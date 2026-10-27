# OpenX6X00 audit and possible improvements — 26 October 2026

Reviewed revision: `e4f0dc7b08f7e95fd53495baeb55d35c19857cf1` (`v0.3.6`). **Actual inspection date: 27 October 2026.** The filename uses the date requested; this report includes the current tree, including its 27 October commits. Baseline: [24 October audit](Codex-Audit-2026-10-24.md), which reviewed `00f91e1` (`v0.3.1`).

The previous fixes resolve many of their demonstrated failure paths. The current review identifies **14 current findings: 9 High and 5 Medium**, including remaining FT8 lifetime problems, pending TCI ownership, browser transmit/logging state, Winlink split operation and Wavelog integration. Improvement ideas follow the findings and account for features already implemented.

Only this report was added to repository content. Application source and configuration were unchanged. The existing ignored host build was rebuilt for validation; new test fixtures and harnesses remained under `/tmp`. No GUI was launched, network listener opened, radio service changed, live update/restore performed or RF transmitted.

## Scope and evidence

The review compared the earlier findings against current native and browser control, TCI, ownership, scheduler, FT8 worker/QSO/UI lifetime, settings, ADIF/log editing, updater and restore. It also inspected the new Winlink and Wavelog integrations, boot restoration, selected hardware integration paths, documentation and current Panel screenshots. Vendored modem algorithms, the kernel, baseband firmware and complete Buildroot images were outside exhaustive review.

**Reproduced** means production code ran in a host harness with temporary files, fake clocks, controlled interleavings or service/hardware stand-ins. Some checks extract selected production functions; others compile whole production translation units. These establish the stated local behavior, not on-radio incidence or RF duration. **Code-established** marks a traced implementation path without complete runtime reproduction. Conditional browser/network and target-specific assumptions are stated explicitly.

**High** denotes a credible unintended or unreleased transmission, memory-access error/crash, falsely acknowledged lost QSO, wrong TX frequency or credential disclosure. **Medium** denotes narrower data-integrity, state, usability or recovery defects. These are engineering priorities, not CVSS ratings. **C01–C14** belong to this report; earlier A/B identifiers retain their meanings.

## What has materially improved

- The browser **Panel is already a native operating interface**: structured spectrum/waterfall, tuning, modes, levels, spots, FT8, CW, logging and optional Screen/Keys. The original proposal to go beyond mirroring has substantially landed. See [Panel](../../webserver/src/x6100_webserver/views/panel.html), [manual](../MANUAL.md#browser-panel), and [desktop](../screenshots/web_panel.png)/[phone](../screenshots/web_panel_phone.png) screenshots. Further work should complete this interface's behavior rather than duplicate it.
- Pairing, roles, control ownership, microphone-source labeling, PTT leases, health indicators, reconnect handling, log receipts and persistent drafts exist. The findings below concern their remaining contracts and edge cases.
- Prior updater/restore, malformed ADIF, inactive-VFO, settings-rollback and web-edit fixes have targeted regression evidence. Current TCI syntax/audio tests build again.
- Winlink, Wavelog, Tailscale, SSH key management, X6200 support, the CW Keyboard app and startup diagnostics are present. Their existence is distinguished from hardware/operational acceptance. In particular, live ARDOP gateway acceptance remains a documented open item.

## Current findings

| ID | Priority | Finding | Evidence |
|---|---|---|---|
| C01 | High | Pending TCI starts can assign TX ownership to the wrong client | Current server reproduced with deferred scheduler |
| C02 | High | Delayed browser renewals can re-key an expired PTT hold | Client reproduced; server recovery path traced |
| C03 | High | Two tabs of one paired browser can lose a QSO through ID collision | JavaScript allocation and C receipt handling reproduced |
| C04 | High | FT8 can free a successfully created worker before it starts | AddressSanitizer reproduction |
| C05 | High | GUI QSO reset can free the candidate being saved by the decoder | AddressSanitizer on unchanged QSO implementation |
| C06 | High | Queued FT8 waterfall jobs can access a deleted widget | AddressSanitizer on production callback bodies |
| C07 | High | Winlink can transmit on the other VFO instead of Pat's gateway frequency | Production frequency/refusal decisions reproduced |
| C08 | High | Wavelog forwards its stored key to a caller-selected destination | Credential forwarding reproduced; hostile-website path conditional |
| C09 | High | Space on ordinary focused buttons requests PTT | Production keyboard handler reproduced |
| C10 | Medium | Unicode log editing corrupts ADIF lengths and adjacent fields | Production parser/editor reproduced |
| C11 | Medium | Wavelog deduplication suppresses distinct contacts and corrections | Production collection reproduced |
| C12 | Medium | Editor outcomes hide newer or failed unsaved drafts | Production edit-flow state reproduced |
| C13 | Medium | A rejected tune can remain displayed as confirmed radio state | Client mutation reproduced; rejection/state path traced |
| C14 | Medium | Boot backup's regular-file filter does not constrain extracted member types | Host extraction reproduced; target matching source inspected |

### C01 — Pending TCI key requests overwrite the successful transmitter's owner

Sources: [admission and owner assignment](../../src/tci_server.c#L879), [GUI key application](../../src/tci_server.c#L523), [refusal cleanup](../../src/tci_server.c#L667), [disconnect cleanup](../../src/tci_server.c#L777).

Admission checks actual TX and the audio-session owner, but omits an already pending physical `ptt_owner`. Two clients can send `trx:0,true;` while the radio still reports RX and before the GUI drains its queue. Both requests are accepted; the second overwrites the owner. The first callback keys, the second is refused, and the owner remains the second client. The queued command carries no requesting-client identity to reconcile the result.

**Reproduction:** the unchanged current TCI server, control-owner code and existing radio stubs ran with deferred scheduler delivery, without sockets. Two requests produced `ptt_owner=1, pending=2`. After application, PTT was on; client 0's release left `ptt=1`, while client 1's release unkeyed it. Disconnect cleanup also consults the overwritten owner. Independent protections or the 180-second limit may eventually end TX; the original keying client's release is ineffective.

**Direction:** reserve pending TX ownership before enqueueing, carry session/generation through application, and bind cleanup to the successful key mechanism. Refused or cancelled starts must not change another session's ownership.

**Acceptance:** interleave two starts, local takeover, cancellation before application, refusal, disconnect and explicit release. Only one start may win; its owner must always be able to release. B01's earlier authorization-order path now passes and is distinct from this race.

### C02 — A delayed renewal can become a fresh PTT after lease expiry

Sources: [stalled-link handling and send](../../webserver/src/x6100_webserver/static/js/radio_link.js#L94), [PTT renewal](../../webserver/src/x6100_webserver/static/js/radio_session.js#L203), [Panel stall callback](../../webserver/src/x6100_webserver/views/panel.html#L869), [server expiry](../../src/remote_server.c#L1889), [renewal/new-key handling](../../src/remote_server.c#L1600).

A missing pong marks the link stalled, but its ready-state-1 WebSocket keeps accepting sends. Panel and Screen only refresh their displayed status on that event; PTT renewals continue. The server releases an expired held key but retains the connection/control claim. A later `key PTT 1` is indistinguishable from a deliberate new press: it adds the held key again and resets its start time.

**Scenario:** hold radio-microphone PTT, pause delivery beyond the three-second lease but below socket expiry, then resume TCP delivery. Buffered renewals can re-key after the server's protective unkey, without a new operator press. A later release may end it again, but does not prevent that intervening re-key. Stale tuning commands can likewise arrive after a gap.

**Evidence:** the complete production link component with a fake socket/missing pong returned `stalled=true`, `sentWhileStalled=true`, and queued `key PTT 1`. The matching server expiry/re-entry path is code-established; an end-to-end network/RF experiment was not performed.

**Direction and acceptance:** distinguish a fresh press from renewal using a hold identity; expiry must invalidate that hold. Retire transient RF intent on stall and reconcile a fresh snapshot before accepting another deliberate press. Test asymmetric pauses, buffered catch-up, expiry and recovery: no renewal of an expired hold may start TX again.

### C03 — Same-browser tabs reuse log operation identities

Sources: [per-page preferences](../../webserver/src/x6100_webserver/views/panel.html#L297), [log counter](../../webserver/src/x6100_webserver/views/panel.html#L792), [shared browser token](../../webserver/src/x6100_webserver/static/js/radio_session.js#L15), [session identity](../../src/remote_server.c#L1092), [deduplication](../../src/remote_server.c#L1244).

Each Panel tab loads `pref.logSeq` once and increments its private copy. Tabs share the paired browser token, so they also share the server's receipt namespace. Two already-open tabs can both allocate ID 1 for different contacts. The server deduplicates solely by session and number and returns the first contact's success for the second.

**Reproduction:** production preference/allocation snippets in two tab stand-ins produced IDs `{1,1}`. Current extracted C receipt/dispatch functions received different callsigns with that shared session/ID; only one write was dispatched, but the second tab received `logged 1 ok`. The downstream disk writer was a counting stand-in. This establishes false acknowledgement/lost dispatch without requiring concurrent RF operation or two control owners.

**Direction:** use stable, globally unique operation identities across tabs and reloads, and bind each receipt to its payload. Reusing an identity for different data should return a conflict rather than another contact's success.

**Acceptance:** two tabs, reload, lost acknowledgement, unavailable local storage and receipt eviction must preserve two different contacts exactly once each. B07's original cross-session collision and automatic unknown-result retry are addressed; this is a residual same-session path.

### C04 — FT8 worker teardown can precede the child's first instruction

Sources: [child running flag](../../src/ft8/audio_worker.c#L176), [start/stop/destroy](../../src/ft8/audio_worker.c#L281), [app lifecycle](../../src/ft8_window.c#L323).

`audio_worker_start()` creates a pthread successfully, but only the child sets `thread_running=true`. Stop joins only when that flag is already true. If the parent stops/destroys before the child is scheduled, it skips the join and frees the worker, its buffers and synchronization objects. The child then writes into freed memory on entry.

**Reproduction:** a start wrapper delayed child entry until after parent destruction, a valid scheduling interleaving. Extracted production worker/start/stop/destroy code printed `after successful start: thread_running=0`; AddressSanitizer then reported heap-use-after-free at the child's first store, corresponding to `audio_worker.c:181`. Unrelated DSP operations were stubbed. Fast app/protocol changes can expose this lifecycle; no frequency of occurrence on hardware is claimed.

**Direction and acceptance:** distinguish parent-owned successfully-created/joinable state from child progress. Every successfully created thread must be joined exactly once before resources are released. Test immediate start/stop, delayed scheduling, creation failure, double-start rejection and repeated teardown under a sanitizer.

### C05 — GUI resets can delete the FT8 candidate currently being saved

Sources: [worker decode handoff](../../src/ft8_window.c#L1164), [CQ reset](../../src/ft8_window.c#L672), [Hound reset](../../src/ft8_window.c#L911), [candidate save](../../src/ft8/qso.cpp#L85), [candidate deletion](../../src/ft8/qso.cpp#L314).

The decoder and GUI mutate one QSO processor without operation-wide ownership. Completing a QSO calls an external save callback and then writes `_saved=true` into the candidate. While the callback performs logging work, CQ/Hound GUI actions can reset the processor and delete that candidate. Returning from the save callback then writes to freed memory.

**Reproduction:** the entire unchanged production `qso.cpp` was linked with minimal grid/modifier stand-ins. A normal QSO was advanced to RR73 on a worker; its save callback paused while the main thread called the same reset method used by the UI. AddressSanitizer reported a heap-use-after-free write at `qso.cpp:88`, freed by reset at line 316.

**Direction:** give the worker sole ownership of QSO/TX-message state and submit GUI actions as commands, or implement a complete locking/lifetime protocol. Account for callbacks and shutdown: introducing a blocking GUI call while the GUI joins the worker would create another dependency problem.

**Acceptance:** delay automatic saving while toggling CQ/Hound, selecting a caller and force-saving. Candidate lifetime and QSO outcomes must remain consistent. This is a stronger state-ownership problem related to B05, independent of direct widget calls.

### C06 — Stopping the FT8 worker does not retire queued waterfall callbacks

Sources: [row enqueue](../../src/ft8_window.c#L1247), [unguarded delivery](../../src/ft8_window.c#L1234), [app destruction](../../src/app_window.c#L125), [widget dereference](../../src/widgets/app_waterfall.c#L117).

The worker queues a heap-owned PSD row through the general scheduler. Its callback unconditionally uses the global `waterfall` widget. Closing FT8 joins the worker and then deletes the widget, but accepted scheduler callbacks are not retired by that join. Dropping queued object events in `close_window()` does not remove these general callbacks. An old row can therefore hit a freed widget, or a newly reopened app instance.

**Reproduction:** extracted production enqueue/delivery/waterfall functions queued a row, deleted a stand-in widget as teardown does, and drained the accepted callback. AddressSanitizer reported heap-use-after-free at the first read corresponding to `app_waterfall.c:120`. This establishes the deferred-lifetime problem, not full LVGL rendering behavior. A rejected enqueue also leaks the row allocation because its return value is ignored.

**Direction and acceptance:** attach app generation/lifetime to jobs, discard expired jobs while freeing payloads, and reconcile outstanding jobs before resource deletion. Test close while rows are pending, close/reopen before delivery and queue saturation. A Boolean `run` check alone does not distinguish reopened instances.

### C07 — Winlink's simplex check misses ordinary A/B split

Sources: [Winlink refusal checks](../../src/winlink.c#L408), [Pat frequency application](../../src/winlink.c#L683), [TX-frequency selection](../../src/radio.c#L404), [TX player](../../src/tx_player.c#L116).

Winlink rejects split only when a nonzero TX shift is configured. Ordinary split, with separate A/B frequencies and zero shift, passes. Pat tunes the active receive VFO; `tx_player` selects `radio_tx_freq()`, which returns the inactive VFO in that split configuration. The bridge can therefore listen on the RMS frequency and transmit elsewhere.

**Reproduction:** current production frequency selection and Winlink refusal functions ran with receive VFO 14.100 MHz, background VFO 14.200 MHz, split enabled, shift zero, and idle radio. Result: `Pat RX frequency=14100000 manual split TX=14200000 refused=no`. The actual code then passes the selected TX frequency to the shared player. This reproduced the decision with parameter/hardware stand-ins; no RF was generated.

**Direction:** define and enforce a complete simplex operating context for Winlink, including ordinary split, effective TX frequency and restoration after the session. Validate the final context at key time rather than only the requested receive frequency.

**Acceptance:** cover A/B active choices, manual split, positive/negative shifts, cross-band tuning and changes between tune and key. The effective TX must equal the gateway frequency or the request must be refused explicitly.

### C08 — Wavelog can send a saved API key to an unsaved destination

Sources: [form-to-helper forwarding](../../webserver/src/x6100_webserver/apps.py#L1253), [test/stations routes](../../webserver/src/x6100_webserver/apps.py#L1325), [helper destination/key fallback](../../rootfs/usr/sbin/x6100-wavelog#L125), [request guard](../../webserver/src/x6100_webserver/apps.py#L34), [trusted-host helper](../../webserver/src/x6100_webserver/apps.py#L569).

The form accepts an arbitrary HTTP(S) URL and an empty key. The helper combines that unsaved URL with the stored key. Testing a different server can therefore disclose the old server's credential. Stations lookup also places the selected key in the destination URL path.

**Reproduction:** the current test route and helper ran with a fake saved key, changed destination and empty supplied key. A capture replacing the only outbound request recorded `https://attacker.example/index.php/api/version` with body `{"key":"FAKE-STORED-API-KEY"}`; the route reported success. No real credentials, DNS changes or HTTP requests were used.

There is an additional conditional hostile-website path: these routes do not use `local_host_ok()`, unlike SSH/Tailscale/Winlink. Matching attacker Host/Origin values pass the general write check if a browser/network permits DNS rebinding. That browser chain was traced, not live-tested. The unrestricted Files route is a parallel path to the stored configuration under those same preconditions; fixing only the connection-test endpoint would not complete the hostname boundary. The manual explicitly permits trusted LAN/tailnet users to download keys through Files, so ordinary access by those users is an intentional product policy.

**Direction and acceptance:** bind saved-key reuse to its saved destination origin and require a replacement key or explicit reviewed destination change. Apply the intended hostile-website/hostname policy consistently across sensitive APIs and file reads. Test changed-origin/blank-key requests, unrelated Host plus matching Origin, and legitimate existing-server tests; no old secret should leave for an unapproved destination.

### C09 — Keyboard activation of ordinary buttons can request transmission

Sources: [Panel document key handler](../../webserver/src/x6100_webserver/views/panel.html#L957), [Screen key handler](../../webserver/src/x6100_webserver/views/remote.html#L293), [session dialogs](../../webserver/src/x6100_webserver/static/js/radio_session.js#L35).

The global handlers exclude text inputs but not buttons, selects, contenteditable elements or every modal. With operating permission/control, Tab to Sound, Light, App or an ordinary Close button and press Space: the handler requests PTT and prevents the button's normal activation. Screen's global Enter mapping can similarly inject MFK while interacting with ordinary controls. Panel's own popup check does not cover the separate session dialog.

**Reproduction:** the production Panel handler, with a focused BUTTON target and Space key, produced `spacePttPresses=1, defaultPrevented=1`. The PTT operation itself was a counting stand-in; no transmission was exercised.

**Direction and acceptance:** scope RF shortcuts to an explicit operating context and honor normal interactive elements and all modal focus. Keyboard-only activation of Sound/Close/settings must never inject PTT or front-panel keys. Cover button/select/contenteditable targets, dialogs and assistive-technology activation.

### C10 — Unicode log edits consume byte counts as character counts

Sources: [ADIF set/delete](../../webserver/src/x6100_webserver/apps.py#L1372), [web parser](../../webserver/src/x6100_webserver/apps.py#L1568), [accepted text fields](../../webserver/src/x6100_webserver/apps.py#L1607), [helper byte parser](../../rootfs/usr/sbin/x6100-wavelog#L193).

The radio/helper count ADIF field lengths in bytes; the web editor applies those lengths to decoded Python characters and writes `len(value)`. Supported non-ASCII NAME/QTH/COMMENT input therefore makes readers disagree and can damage following tags during replacement.

**Reproduction:** for adjacent `<NAME:7>Jürgen<MODE:3>SSB`, the production web parser read `Jürgen<` and missed MODE. Changing NAME to Peter produced `<NAME:5>PeterMODE:3>SSB`, removing the next tag's `<`. Entering Jürgen into an ASCII record wrote length 6; the current byte parser then read `Jürge`. Whitespace can mask the delimiter example but does not fix the wrong newly emitted length.

**Direction and acceptance:** use bounded byte offsets consistently for parsing, editing and writing, with explicit UTF-8 conversion. Round-trip accented/multibyte names, QTH and comments, adjacent tags, deletion and typed fields through native, web and upload readers; unrelated fields and encoded lengths must survive unchanged.

### C11 — Wavelog's duplicate key omits station identity and record revisions

Sources: [key construction](../../rootfs/usr/sbin/x6100-wavelog#L214), [collection suppression](../../rootfs/usr/sbin/x6100-wavelog#L221), [automatic station selection](../../rootfs/usr/sbin/x6100-wavelog#L301).

The key uses remote callsign, date, minute, band and mode. It drops seconds and own-station context, then suppresses records already queued/sent without comparing their content. Automatic station-profile selection occurs later. Report/name/reference/frequency corrections are likewise suppressed, despite the helper/manual describing edited QSOs as new uploads.

**Reproduction:** two valid DL1ABC/20M/SSB contacts at 12:00:01 and 12:00:40, with station callsigns ON6URE and ON6URE/P, yielded one queued contact. Separately, after a QSO was marked sent, changing RST_SENT 59 to 599 and collecting again yielded an empty queue. The fixture changed file size to avoid confusing this with stat caching. Local ADIF remained intact; the external log missed work before any network request.

**Direction:** separate durable contact identity, transport idempotence and content revision. Include own-station context/full time and define an intentional correction policy. The integer-second mtime/size cache at lines 235–236 also merits same-size/same-second edit tests.

**Acceptance:** distinct own-station/same-minute contacts survive; corrections become visible pending work according to policy; overlapping local logs and retry delivery still avoid accidental duplicates.

### C12 — A save response can hide newer unsaved edits

Sources: [editor dirty/outcome states](../../webserver/src/x6100_webserver/static/js/edit_flow.js#L42), [draft preservation](../../webserver/src/x6100_webserver/static/js/edit_flow.js#L65), [memory save callback](../../webserver/src/x6100_webserver/views/memories.html#L113), [CW save callback](../../webserver/src/x6100_webserver/views/cw_messages.html#L59).

Inputs remain editable during Saving, but their change check returns early. On response, rebuild preserves a newer draft; the page callback then unconditionally marks that row Saved, disables Save and removes its navigation warning, even though its value differs from the saved server value. Failed dirty drafts also disable retry and are excluded from the beforeunload selector.

**Reproduction:** original A → submit B → type C while waiting → receive saved B. Production rebuild retained C as Changed; the success callback changed it to `saved`, with `saveDisabled=true, dirty=true`. A failed dirty draft similarly had retry disabled and no failed-state unload protection. DOM/jQuery stand-ins exercised the production functions; no live browser server was used.

**Direction and acceptance:** maintain dirty/revision state independently of request outcome. Confirm the exact submitted version; only mark the current draft Saved when it still matches. Preserve retry/discard and navigation protection after failure. Test editing during save, reordered responses, timeout and retry without requiring another edit.

### C13 — Attempted tuning replaces authoritative display state before acceptance

Sources: [optimistic tuning](../../webserver/src/x6100_webserver/views/panel.html#L514), [optimistic levels](../../webserver/src/x6100_webserver/views/panel.html#L614), [server permission rejection](../../src/remote_server.c#L1423), [set admission](../../src/remote_server.c#L1587), [change-driven state delivery](../../src/remote_server.c#L1318).

`tuneTo()` immediately overwrites `state.freq` before its delayed command is sent. It does not check known permission or send success. Level controls do the same. An unpaired/other-controller request can be rejected, but the error path does not restore the prior state and the server need not resend an unchanged snapshot. The large frequency readout can therefore show an attempted value while the live radio remains elsewhere.

**Reproduction:** current client tuning code changed displayed state from 14.074 to 14.075 MHz while a stand-in authoritative radio stayed at 14.074. Actual event callers are not permission-gated. Rejection without mandatory corrective snapshot is source-established; no full rendered/network experiment was performed.

**Direction and acceptance:** keep pending intent separate from confirmed state, check locally known permissions and reconcile rejection or failed dispatch. Test view-only, denied takeover, app locks, invalid range and queue refusal; rejected values must not look confirmed while Link/meters remain live.

### C14 — Boot restore's name list bypasses its regular-file-only check

Sources: [selection and extraction](../../rootfs/etc/init.d/S04restore_backup_config#L53), especially lines 63–68. The [Bluetooth restore](../../rootfs/etc/init.d/S39btpairings#L16) already recognizes the related danger of selecting tar entries by name/pattern and writes selected bytes itself.

Configuration restore lists regular members, saves their names, then runs a second general tar extraction with that name list. A name does not identify a unique archive member or its type. An archive can contain a permitted regular `root/.ssh/poc` followed by a FIFO or link with the same name; the second extraction selects both. Pattern/descendant matching widens the mismatch further. On a freshly written card the advertised regular-file-only boundary is therefore not enforced at extraction.

**Reproduction and limit:** a temporary archive with that regular/FIFO duplicate passed the script-equivalent regular-name selection; GNU tar extraction returned success and installed a FIFO. The locally available target BusyBox 1.36.1 source was also inspected: selection uses `find_list_entry2()` by name/pattern, while `data_extract_all()` processes links/special types. Its ARM executable was not run. No privileged directory or live backup was touched, and no arbitrary-path write is claimed from this test.

**Direction and acceptance:** validate and extract the same individual regular member, reject duplicate/ambiguous names and special types, and constrain destination creation independently of archive metadata. Test duplicate names, literal wildcard characters, descendants, links, special members and existing destinations with the actual target tar behavior.

## Disposition of the 24 October findings

“Addressed” refers to the previously demonstrated case, not complete subsystem certification. Runtime checks below used current production code; some old extracted harnesses required a different targeted check because their stubs no longer compile.

| Previous ID | Current disposition | Evidence in this review |
|---|---|---|
| B01 | Original authorization-order case addressed | Current complete TCI handler unkeys after local takeover and Listen only in a direct stubbed check. C01 is a different pending-owner race. |
| B02 | Nominal pairing-screen leak addressed in source | `pair_show` establishes suppression; encoder withholds frames from unpaired clients while the code is visible by its deadline. No full GUI pairing experiment. |
| B03 | Original VHF formatting overflow addressed | Current 145.5 MHz formatter check passed under AddressSanitizer. |
| B04 | Original truncated numeric conversion addressed | Current `<CALL:5` guard-page fixture returned zero records without the invalid read. |
| B05 | Partially addressed | Several auto-reply/UI effects now use GUI jobs. `save_qso()` still clears the finder directly on the worker at `ft8_window.c:300`; C05/C06 expose additional ownership/lifetime defects. |
| B06 | Original Field Ops truncated-line overread addressed | Current 1,023-byte malformed-record fixture returned zero records under AddressSanitizer. |
| B07 | Original cross-session collision/retry paths addressed; same-session gap remains | Current cache harness distinguishes browser identities and suppresses repeated dispatch. C03 reproduces different tabs sharing an identity. |
| B08 | Original accepted/rejected upload substitution addressed | Concurrent upload check installed accepted bundle A, rejected B and removed temporary uploads. Reservation precedes the immutable upload write. |
| B09 | Original stale ordinal case addressed | Digest-bound concurrent deletes kept the intended record; missing stale identity returned HTTP 409. |
| B10 | Original renewal-timer cancellation addressed | Existing fake clock produced five key/renewal messages through four seconds with renewal still active. C02 concerns delivery after expiry. |
| B11 | Original inactive-VFO context loss addressed | Background B=7.1 MHz preserved active A/mode, persisted the expected value and switched context correctly on VFO toggle. |
| B12 | Original shared-transaction retry loss addressed | Flush after injected rollback restored persisted volume 42, matching memory 42. |
| B13 | Original concurrent HTTP cluster edits addressed | Two delayed concurrent additions both survived the full mutation lock. |
| B14 | Board key mapping addressed in source | Remote key table is board-specific and supported keys are advertised. No X6200 input/hardware experiment. |
| B15 | Advertised/accepted mode mismatch addressed in source | Panel and TCI mode sets now use SAM/WFM capabilities. Actual backend/RF behavior remains untested here. |
| B16 | Original one-shot completion loss addressed in source | Playback retries enqueueing completion; recording failure notification retries with subsequent audio. Queue/audio stress acceptance was not run. |
| B17 | Original MODE/SUBMODE contradiction addressed | Current web checks round-tripped FT8 and JS8 into the expected paired fields; reference-edit regressions also passed. |

Selected original A02/A03 updater interruption/repeated-undo checks and A10/A22 stale-backup/surviving-writer rejection checks also passed. Earlier A findings were not all re-exercised exhaustively in this pass.

## Possible improvements for the current product

These are ideas only. Effort is relative and needs design/target validation. They extend the current Panel, native apps and service integrations; the old remote redesign is not being proposed again.

| ID | Priority / effort | Idea grounded in current behavior | Useful acceptance criterion |
|---|---|---|---|
| I01 | High / Medium | **Complete keyboard and dialog accessibility.** Use real focusable actions for frequency, levels, spots and decode rows; label log inputs; trap/restore dialog focus and handle Escape. Scope RF shortcuts as in C09. Sources: `panel.html:173,199–202,250–252,649–657,724–736`, `radio_session.js:35–47`. | Tune/listen/log/stop/pair works with keyboard and screen reader; ordinary activation never injects PTT/MFK. |
| I02 | High / Medium | **Compact phone operation.** Keep App/fullscreen, but collapse introductory/navigation/status detail after setup. Offer an operating strip with actual RX/TX, Stop/PTT and the chosen log/task action. The current phone screenshot places extensive header/status and waterfall content before frequent operating actions. | Narrow portrait, software keyboard and 200% text zoom retain visible TX/Stop and permit logging without repeatedly scrolling through all tuning controls. |
| I03 | Medium / Small–Medium | **Direct frequency entry and capability-aware filters.** The frequency popup says “type” but uses an internal keypad string; provide a labeled unit-aware text field with paste and resolved-Hz preview. Remove the implicit 60,000-kHz unit switch. Derive symmetric/preset filters for SAM/WFM from capabilities rather than only AM/NFM numeric cases. Sources: `panel.html:470,580–607`. | Keyboard, paste and touch resolve to the same supported frequency; invalid input preserves confirmed state; X6200 passbands match the selected mode. |
| I04 | Medium / Medium–Large | **Complete the Field Ops companion.** Reuse existing activation state and browser log submission: show reference/program, station profile, count, last durable QSO and separate spot/upload result. Add a context-preserving last-contact correction. Sources: Panel Log at `panel.html:250–254`, existing native Field Ops state and log handling. | Reconnect resumes the same activation; uncertain delivery causes no duplicate; stored frequency/mode/UTC/reference can be inspected. |
| I05 | Medium / Medium | **Make Wavelog's outbox inspectable.** Show destination, station profile, contact identity, pending/sent/rejected status and corrections per record. Separate transport retry from intentionally revised contact upload. Build on the existing durable queue rather than adding another uploader. | An operator can explain a missing upload, retry it safely and verify whether a correction is pending; local save and external acceptance are distinct. |
| I06 | High / Medium–Large | **Winlink preparation and recovery.** Explain effective RX/TX frequency, split/simplex policy, USB-D/filter, audio/active-app ownership and gateway state before connection. Keep service-running, modem-ready, connected-to-RMS and message-delivered as separate outcomes. Reuse Pat's mailbox and supervisor. | Simulated startup/disconnect errors have a next action; ordinary split is handled safely; live ARQ, turnaround, levels and protection acceptance is documented separately for both radios. |
| I07 | Medium / Medium | **Task-specific station readiness.** Extend transport health with station identity, clock source/last synchronization/unknown accuracy, writable storage/free space, selected audio source and active modem/application. Link directly to the relevant setting; listening must remain usable offline. | Users can identify why FT8/Winlink/CW cannot proceed without inferring readiness from WiFi or a running process alone. |
| I08 | Medium / Small–Medium | **Persistent outcomes and honest uncertainty.** Keep a bounded local history for logs, spots, updates, microphone and TX events. A PTT release timeout should say RX is unconfirmed instead of returning its button to idle solely after 1.5 seconds (`radio_session.js:225–231`). The separate Panel TX header may still be correct; this is a consistency improvement, not proof that unkey failed. | A later Connected message cannot erase an unresolved failure. Lost RX confirmation retains the last known TX and an actionable explanation. Diagnostics copied on request omit tokens/secrets. |
| I09 | High / Medium | **Turn ownership/lifetime contracts into maintained regression checks.** Keep the repaired TCI audio tests; update the older audit extraction stubs and register public checks in one discoverable runner/CI. Add deterministic deferred-start, queued-close, two-client/two-tab, Unicode and rejected-command cases. | A fresh checkout runs the documented host checks without private files or stale helper declarations; expected failures are distinguishable from harness compilation errors. |

Before adding another feature family, prioritize C01–C09 and truthful saved/confirmed state. The main usability gain now comes from completing the behavior already presented to operators.

## Validation performed

| Check | Result / limit |
|---|---|
| `cmake --build build-host --target x6100_gui params_sqlite -j 4` | Passed for the stub backend. Existing enum-operation warnings remained. No GUI execution. |
| Current `tools/tci` copied to `/tmp`, `make syntax audio_test` | Passed, including current server/control-owner syntax and audio conversion tests (`AUDIO OK`). No listeners. |
| `ctest --test-dir build-host -N` | Zero registered tests; standalone suites exist outside CTest. |
| Saved `repro-2026-10-24/run.py` | Web/log/updater/backup/PTT checks passed, then the generated B01 C harness failed to compile because its copied TX/client stubs lack current audio-format fields/helpers. The suite as shipped did **not** finish. |
| Remaining saved cases run separately | B03/B04/B06 sanitizer inputs, B07 cache case, B11 VFO and B12 rollback expectations matched current fixes. Current-server targeted B01 check also passed. |
| New native interleaving checks | C01 reproduced against current server; C04/C05/C06 intentionally terminated on the described AddressSanitizer use-after-free reports. |
| New browser component checks | Current production JavaScript with fake transport/clocks/DOM reproduced stalled sending, tab-ID collision, button Space-to-PTT, dirty-state outcome and optimistic tuning behavior. No rendered-browser/RF acceptance. |
| New web/helper checks | Extracted production routes/helper functions with request/outbound stand-ins reproduced saved-key forwarding, Unicode corruption and Wavelog collection omissions. No network calls. |
| Winlink split decision | Production refusal/frequency-selection functions reproduced receive 14.100 MHz versus allowed TX 14.200 MHz under manual split. |
| Boot archive type selection | Regular/FIFO duplicate selected by name and installed as FIFO in temporary GNU tar extraction; target BusyBox selection/type-handling source inspected separately. |
| Python syntax | Fourteen webserver/helper/bundler Python files parsed successfully. |
| Shell syntax | `buildroot/local-build.sh`, `host/build.sh` and selected changed boot scripts passed `bash -n`. This is syntax checking, not a target-shell boot test. |

The old harness failure is a regression-maintenance gap, not a firmware compile failure. The repaired current TCI tests and the older audit extraction harness are separate tools. New temporary reproductions were not committed as source additions in this documentation-only request; inputs, interleavings and expected outcomes are recorded above.

No complete X6100/X6200 image build, sudden-power-loss test, live browser audio/microphone test, on-air ARDOP session or real-device concurrency stress was performed. A successful stub build cannot certify baseband/control-library semantics or hardware timing.

## Follow-up risks and scope boundaries

- **Power-loss recovery:** current process-interruption checks pass. Updater previous-file copies and affected rename directories still need actual power-loss durability tests; those checks do not establish what survives storage power removal. Settings still choose `synchronous=OFF` with WAL, an explicit durability/performance tradeoff.
- **Pairing-display timing:** hiding unpaired frames now closes B02's nominal path. Suppression starts before queued GUI display rather than being tied to actual secret visibility. A sufficiently delayed display merits a timing test; this was not promoted to a new credential-leak finding.
- **Winlink concurrency:** `rig_do_tune()` uses a shared completion counter and leaves a queued tune alive after its 700-ms wait times out. Review cancellation, late application and concurrent pending-tune consumers before claiming command-level completion guarantees. The definite split defect is C07; these further cases were not reproduced here.
- **Native/UI ownership:** most old FT8 visual effects were moved into GUI jobs, but the remaining direct finder clear and generation-free callbacks show the migration is incomplete. Fixing only the three reproduced lifetime windows would not establish thread safety for every digital-mode app.
- **Known roadmap items:** FT8 CQ hunting/date-window policy, WiFi static addressing, stock-base CESSB and Mercury are already in [TODO.md](../../TODO.md). Winlink support itself is implemented; its documented missing on-air ARQ acceptance is not a newly discovered absence. Multiple browser panels still share one physical RX/TX chain.
- **Backup/operator expectations:** newly added services have different secret/state persistence rules. Make the existing inclusion/exclusion policy visible before export/restore, especially Winlink credentials, Wavelog configuration and network/SSH archives. Changing those policies is a separate product decision.

## Suggested future order

1. Close pending/stale TX ownership and keyboard activation paths, and enforce Winlink's effective TX context: C01, C02, C07, C09.
2. Establish FT8 worker, QSO and deferred-widget lifetime ownership: C04–C06.
3. Preserve operation identity, credential destination and log integrity: C03, C08, C10, C11, C14.
4. Make dirty/pending/confirmed state truthful, then improve compact operation and service workflows: C12, C13 and I01–I08.
5. Preserve each corrected case in maintained public checks and perform separate device/RF acceptance: I09.

All remedies, acceptance criteria and improvements are suggestions for later work. None was implemented by this review.
