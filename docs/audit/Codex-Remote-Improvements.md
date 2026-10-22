# Codex Remote Improvements

Reviewed 22 October 2026 at `d6bf443ac07cb7978a4c7499decea25d95a25c90`.

Ideas only: no implementation or configuration changes. This review used the repository's implementation, manual and repository screenshots; it did not measure a live radio, browser audio or network performance.

## Main recommendation

Build a browser operating workspace around radio tasks and structured radio state. Keep the current front-panel mirror as a useful compatibility/support view. On a laptop, the browser can show tuning, spots, logging and messages together. On a phone, it can show a large frequency, the next useful action, audio controls and a reachable PTT. Both can operate the same authoritative radio without navigating an 800×480 menu image.

This is more than a CSS redesign: a browser-native interface needs a stable radio control/state contract, explicit session ownership and structured access to the applications. Reuse TCI and the existing configurator APIs wherever they already fit; add only the missing capabilities. The [general usability document](Codex-Possible-Improvements.md) covers first-run, naming and editor improvements, so the focus here is the remote operating product. Resolve the TX/lifetime/recovery defects in [the audit](Codex-Audit.md) before expanding operating scope; the existing protective mechanisms listed below have implementation gaps, including A01.

## Current baseline: what exists and what it implies

| Existing capability | Evidence | Implication for a new remote |
|---|---|---|
| JPEG screen stream, up to 10 fps; key press/release and relative knob events | `src/remote_server.c:9-21,58-66`; `views/remote.html:104-123,163-231` | Good complete fallback, but actions depend on the current on-radio screen and button pages |
| Fixed 800×480 or portrait canvas with external physical-key replicas | `views/remote.html:5-12,55-80` | Browser scaling shrinks radio text; the interface does not gain browser-native semantics or accessible screen contents |
| Keyboard shortcuts, wheel tuning, hold PTT, fullscreen, reconnect and release on blur | `views/remote.html:155-175,189-231,304-309` | Preserve useful shortcuts and hold-to-talk behavior in the new workspace |
| Separate TCI WebSocket for RX audio and browser-microphone TX | `views/remote.html:234-301`; `src/remote_server.h:4-6` | An apparently working picture does not establish audio/control readiness; display each subsystem's state |
| TCI frequency, mode, filter, AGC, RIT, drive, PTT and meters already implemented | `src/tci_server.c:395-419,516-643,953-979`; `docs/MANUAL.md:1753-1760` | Core native controls and meters can reuse existing radio capabilities |
| One transceiver, one receive channel, one current frequency exposed by TCI | `src/tci_server.c:398-407` | Multiple panels or viewers do not create independent receivers; current TCI does not expose two independently controllable VFOs/split |
| TCI has TX ownership, disconnect/audio-silence handling and a TX time limit | `src/tci_server.c:124-134,502-512,581-605,725-749` | Build on existing protection instead of assuming none exists; complete the policy across browser key injection, local controls, CAT and applications |
| Structured web configuration, memories/channels, logs, gallery, Field Ops configuration | `webserver/src/x6100_webserver/apps.py`; `views/memories.html`, `views/logs.html`, `views/field.html` | Reuse these stores and operations; an operating workspace need not rebuild configuration or file management |
| Configurator socket applies display/Ethernet changes through the GUI scheduler | `src/cfg_socket.cpp:6-12` | A useful pattern for safe GUI-thread commands, but not yet a complete semantic operating API |
| No login; same-origin WebSocket check; remote and TCI enabled by default | `src/remote_server.c:20-21`; `apps.py:265`; `docs/MANUAL.md:1715-1725,1741-1750` | Real viewing/control/TX permissions require a new explicit product policy; an Origin check is not user authorization |
| HTTP/HTTPS and extra secure WebSocket proxy ports | `webserver/src/x6100_webserver/__main__.py:103-107,151-155`; `views/remote.html:95-100` | A single secure origin would simplify certificates, microphone setup and deployment |
| Documented moving-waterfall traffic roughly 300–500 kB/s | `docs/MANUAL.md:1712-1713` | A useful baseline for comparison, not a measured result of this review |

In this document `views/...`, `apps.py` and `__main__.py` refer to files under `webserver/src/x6100_webserver/`.

## Three workspaces, one radio

### 1. Desktop operating desk

A persistent header shows station identity, radio connection, last confirmed frequency/mode, RX/TX state, active controller and any protection reason. Below it:

- **Tuning area:** large directly editable frequency, explicit tuning step, mode, filter, RIT, AGC and TX power. Frequency digits support keyboard/wheel adjustment. A/B and split appear only when a semantic API exposes and validates them; the present TCI announcement is not enough.
- **Spectrum/waterfall:** a wide independently scalable plot with readable frequency labels, selected passband, cluster overlays and click-to-tune. Zoom/pan within the actually available RF span; never imply the whole HF spectrum is simultaneously sampled.
- **Spots and memories:** searchable/filterable lists beside the plot. Selecting a spot shows callsign, frequency, mode, spot age/source and whether it is already worked in the relevant log context. Deliberate Tune changes the radio; a preview does not. Offer a short back-stack to return to the previous frequency/mode.
- **Log and messages:** compact QSO entry beside recent QSOs, CW/voice messages and current application status. A contact can be selected, worked and logged without replacing the entire operating view.
- **Audio and transmit:** separate local playback level, selected microphone/device, microphone input meter, actual TX meters, hold PTT and a persistent Stop action.

Allow optional panels and a few presets; start with a sensible layout. Do not ship a complicated dashboard editor before the basic workflow is reliable. Browser layout preferences can stay per device; radio mode/frequency/power remain shared radio state.

**Example workflow:** choose a recent spot → inspect it → tune once → listen → hold PTT with the named microphone → log with the radio-confirmed frequency/mode/UTC → return to the previous frequency. No APP menu traversal is required.

### 2. Phone and tablet operating view

Use a browser-native portrait layout rather than a smaller front panel. Keep frequency/mode, audio readiness, actual RX/TX and connection state near the top; make Listening, Spots, Log and Messages quick sections. A stable bottom area can hold PTT and Stop when appropriate. Test it with the software keyboard open so logging does not hide the TX indication or stop control.

Use a numeric frequency input, explicit step buttons and a simple linear fine-tune control. A decorative circular knob can remain optional for users who like it; it should not be the only precise way to tune. Do not make scrolling the page accidentally tune the radio.

Provide a listening layout with PTT absent until control/TX permission is granted. A field layout can prioritize reference, last QSO, counts, CQ and Log. A tablet with keyboard can keep the log or spots beside the tuning view. Preserve keyboard shortcuts with an explicit active-radio focus mode and a visible shortcut reference; typing a log or activating a normal button must not accidentally key TX.

**Acceptance:** on a narrow portrait screen the operator can tune, hear, stop and log without horizontal page scrolling. Software keyboard appearance does not conceal active TX. Controls use labels and sufficient touch targets, and core tasks work at 200% text zoom and with keyboard-only access.

### 3. Field activation companion

Extend the existing Field Ops state into a phone-friendly operating panel: current program/reference, station callsign/profile, position source/age, activation progress, CQ state, last saved QSO, next/last spot and QRT action. Reuse the radio's existing activation persistence, spot delays and log handling (`docs/MANUAL.md:1287-1294,1333-1359,1381-1436`).

A phone is particularly useful as a keyboard, map/reference browser and logbook while the radio remains the RF engine. Phone location should be opt-in and labeled as the phone's location; preview it before replacing station information. Map tiles must have a list/offline fallback so internet loss does not block the activation.

If offline draft logging is later added, give each draft an ID and label it “Not yet saved on radio”. Reconcile with durable radio acknowledgments on reconnect. Do not replay PTT, CQ, spot publication, frequency changes or End activation automatically. A stale automatic spot for an old frequency/reference would be misleading; every external publication needs current state and a deliberate retry policy.

**Acceptance:** a dropped phone connection does not lose the radio's activation; uncertain log delivery can be retried without duplicate QSOs; locally drafted contacts and radio-saved contacts are visibly distinct.

## R1 — Direct radio controls with a versioned state contract

**Priority: first foundation. Effort: medium/large.**

For core settings, use semantic operations such as setting a frequency/mode/filter rather than sequences of “press APP, then F2, then rotate MFK”. Retain front-panel key injection only in the mirror view or a clearly identified compatibility action. Browser operations should have the same meaning regardless of which app is displayed on the radio.

Proposed contract requirements:

- An initial snapshot includes capabilities, units/ranges, confirmed RF state, current activity, controller/TX owner and a monotonic state revision/session generation.
- Incremental changes keep every viewer current, including changes from a physical knob, TCI client, CAT or application. Reconnect gets a fresh snapshot and detects a radio restart.
- Commands have an ID and explicit accepted/applied/rejected outcome. The browser may show “pending”, but claims success only after the resulting state is confirmed. Include rejection reasons such as busy, not supported, invalid value or protection active.
- Absolute set operations are repeatable; relative tune operations need ordering/sequence rules so retries do not tune twice. Coalesce high-rate knob/drag changes and keep stop/release operations ahead of cosmetic updates.
- Group state-dependent changes where atomicity matters. For example, a spot or log records the frequency/mode/reference actually confirmed for that action, not the state from a stale screen.
- Validate ranges/capabilities in the authoritative radio service. The browser's disabled button or range check is presentation, not enforcement.

Reuse TCI for its supported controls and compatibility. Decide whether the browser consumes TCI directly or a shared operating service adapts it. Either way, avoid separate implementations of frequency validation, TX arbitration, settings persistence or application state. Existing `put_cmd`/scheduler integration (`src/tci_server.c:292-355`) is a starting pattern, not proof that the complete contract exists.

**Acceptance:** a browser action succeeds from any native app screen; a physical knob change appears in all viewers; a rejected or delayed change is never displayed as confirmed; reconnect cannot apply an old command to a restarted radio.

## R2 — Spectrum as data, with the browser doing the drawing

**Priority: after core controls; high visual value. Effort: large.**

Expose compact spectrum bins and/or waterfall rows with capture time/sequence, center frequency, span/sample metadata and level information. Draw them in the browser so labels, overlays, selected passband and size adapt to the device. The existing mutex-protected `dsp_spectrum_snapshot()` (`src/dsp.cpp:543`) provides a concrete starting point for a spectrum-data tap; `src/spectrum_view.c`, `src/waterfall_view.c` and `src/widgets/app_waterfall.c` contain related display handling. This is internal infrastructure, not an existing production browser spectrum API.

Send one radio-derived stream to interested viewers rather than performing a separate expensive FFT or JPEG encode for every client. Negotiate an explicit update rate/resolution and drop stale visual updates instead of building a long queue. Prioritize control/stop and audio over waterfall appearance. Offer modes such as Controls only, Audio + controls, Low-data waterfall and Full waterfall. A paused plot must visibly say Paused; a lost feed must visibly show its age.

A lightweight first release could retain a cropped/independent waterfall image while native DOM controls arrive. It should be an explicit interim step, not presented as structured spectrum. Full complex IQ, arbitrary historical RF replay and multiple simultaneous receiver slices are separate, much larger features. Do not imply they follow automatically from this proposal.

**Acceptance:** labels remain readable when resized; click-to-tune uses the frequency metadata of the displayed frame; a slow viewer cannot stall the radio or other viewers; compare CPU, memory, traffic and control latency with the existing JPEG baseline under the same live workload.

## R3 — Session roles and control ownership

**Priority: before extending TX or multi-user operation. Effort: large.**

The current network-trust model is explicit in the manual. A richer remote should support View/listen, Operate and Admin capabilities, with TX permission explicit. One practical local-first flow is pairing a browser using a short-lived code displayed or initiated on the radio, then remembering a revocable session. Include an easy physical “Disconnect/disable remotes” action and a clear list of connected sessions.

Use one controlling session or an explicit handover/lease for RF-affecting actions; additional viewers can listen and follow. Show “Controlled by this browser”, “Controlled locally”, “TCI client active” or “View only” as appropriate. Allow a local-control priority/takeover policy that the operator can understand. Avoid silently stealing the frequency while another operator is transmitting.

This policy must cover every path to RF commands: browser semantic controls, mirror key injection, TCI, CAT and autonomous apps. A browser-only lease is a cosmetic lock if another path can bypass it. Existing TCI TX ownership (`src/tci_server.c:581-605`) solves part of a transmit session; it is not a complete authorization/control policy.

Keep protocol compatibility in mind: a legacy TCI program may not support browser pairing. Document whether it is allowed as a trusted local client, restricted by network/interface policy, or enabled by a local compatibility setting. Do not change its behavior accidentally while adding browser accounts. Any future internet access mode should be a separately scoped design; this proposal does not assume port forwarding or a cloud relay.

**Acceptance:** a viewer cannot tune, key or alter configuration; two clients cannot both own TX; revoke/takeover has an observable effect on existing sockets; switching to the mirror does not bypass permissions.

## R4 — Transmit as a confirmed, bounded session

**Priority: first foundation with R1/R3. Effort: medium/large.**

Make PTT a visible state machine: Not ready → Ready → Requesting TX → Confirmed transmitting → Releasing → Confirmed RX, with separate busy/rejected/protection states. The current browser sets `txOn` and button appearance before hardware acknowledgment (`views/remote.html:284-293`); richer state can distinguish intention from result.

Show the audio source before allowing TX. “Use browser microphone” and “Use microphone at radio” are meaningfully different operations. A missing/denied/disconnected browser microphone should not silently fall back to the radio microphone. Provide a microphone readiness/input-level check that does not key RF.

Build on existing TCI owner checks, disconnect handling, silence timeout and 180-second TX limit (`src/tci_server.c:69-72,502-512,725-749`). Consider a short server-enforced renewable transmit lease for browser voice so frozen tabs, dropped WiFi and lost keyup are bounded even when the socket does not close. Choose timeout values from live audio jitter tests and operating needs; an arbitrary aggressive timeout could chop normal speech.

Hold-to-talk remains the default. Releasing a pointer/key, leaving the active operation view, permission revocation, device loss or relevant transport loss should end the browser's own TX session. Server-side expiry is essential because unload/blur callbacks are not guaranteed. Do not automatically re-key on reconnect or restore a held Space/PTT state. Avoid a general “panic stop” that falsely promises to cancel hardware PTT/VOX beyond software control; the manual already distinguishes those limits (`docs/MANUAL.md:526-539`).

For CW/voice messages and scheduled digital TX, define whether disconnect stops, finishes the current message or continues a radio-owned task. Display that policy before arming it, and provide a stop action for exactly the jobs the session owns. “Stop transmission” and “Cancel future CQ/beacons” should have visible outcomes.

**Acceptance:** controlled tests cover pointercancel, lost keyup, tab freeze, phone lock, WiFi loss, audio loss, browser crash, ownership transfer and radio restart. TX stops within the chosen bounded server policy, hardware-confirmed RX is displayed, and no automatic re-key occurs after reconnect.

## R5 — An audio console with practical diagnostics

**Priority: first operating release. Effort: medium; advanced transport large.**

Use separate controls and state for RX playback, local speaker level, microphone permission, selected input, microphone level and actual RF meters. Add a short nontransmitting microphone check and clear messages for HTTPS required, permission denied, device unplugged, audio context suspended, TCI unavailable and audio stream stalled. Make a user gesture the explicit start of playback/permissions rather than hiding browser restrictions.

The current path is PCM over TCI and a browser ScriptProcessor for microphone capture (`views/remote.html:240-254,284-292`). Keep a reliable LAN version first, with bounded jitter buffers, fresh audio after reconnect and an AudioWorklet-based capture/playback path evaluated for smooth operation. Do not replay buffered old speech when leaving TX or reconnecting. Monitor clipping and show RX/TX delay/drop indicators in an optional diagnostics drawer.

Separate voice processing choices from future digital-audio paths: speech noise suppression/AGC may help voice but must not silently process digital-mode signals. Remote CW timing should remain generated/keyed on the radio; sending network-timed individual elements is a different latency-sensitive project. Text queues and templates are a more realistic first remote CW workflow.

If WAN use becomes a real requirement, evaluate an optional WebRTC/Opus path against the existing LAN path for latency, packet loss, bandwidth and CPU. It brings signaling, certificates and possibly NAT traversal; it is not a free performance upgrade or necessary for the first local-network redesign. Preserve TCI for existing clients.

**Acceptance:** RX volume changes do not change radio speaker or TX microphone gain unless explicitly selected; microphone loss is obvious and bounded; audio queues do not grow without limit; both voice and receive-only sessions recover cleanly on supported target browsers.

## R6 — Reconnect and degraded connectivity that operators can trust

**Priority: first foundation. Effort: medium.**

Treat control, spectrum, RX audio, microphone and application data as independently healthy/stalled/offline. Show last confirmed radio state and age. A frozen waterfall with a working WebSocket must not look live, and a live picture must not imply that PTT/audio works.

Reconnect with backoff, fresh capability/state snapshot and a new session generation. After a radio restart or ownership loss, clear pending transient actions and require a deliberate TX action. Preserve browser layout and unsent text drafts. Persisting safe local drafts is useful; replaying frequency changes, PTT, CQ, upload, spot or End activation after a connection gap is not.

For a QSO or other durable save, use a client operation ID plus a server result so uncertain delivery can be reconciled without duplicate entries. A browser reload should not reset that pending-save identity. Updates and network changes should have distinct progress/reconnect views with the next expected address and an option to return to view-only.

**Acceptance:** pull the network during each operation and verify that the resumed state matches the radio; no stale RF/external-publication commands are replayed; a save submitted before disconnect is either found on the radio or remains explicitly pending for review.

## R7 — Native application panels instead of remote menu traversal

**Priority: after the core radio desk. Effort: large; develop one app at a time.**

Expose structured state/actions from existing radio applications, not duplicate decoders or automation in JavaScript:

| Panel | Browser-native benefit | Required extension beyond current mirror |
|---|---|---|
| Cluster and memories | Search/sort/filter beside waterfall, preview Tune, return to previous station | Live spot/state access and semantic tune action; reuse existing cluster/channel data |
| CW and voice messages | Named send buttons, macro expansion preview, queue/duration, cancel | Semantic queue/send/stop and status; reuse radio keyer/player |
| FT8/FT4/FT2 | Readable/copyable decode table, selected QSO, next message/slot and clear automation state | Decoder event subscription and authoritative application commands |
| JS8 and text modes | Full keyboard editor, searchable inbox, threads/queues | Structured messages/queues plus delivery/TX state; modem remains on radio |
| Field Ops | QSO entry, reference/context, counts and spot status together | Read/write activation state and idempotent log actions |
| Logs and recordings | Recent QSOs, filtered exports, annotate/download/play without leaving the desk | Reuse current logs/gallery APIs; add incremental refresh where useful |

Browser panel lifetime must not silently determine decoder/application lifetime. The radio should report which application is running and which one owns the relevant receive/audio/TX resources. Some combinations may be impossible or too expensive; advertise allowed combinations from capabilities and explain busy conflicts. Two open mode tabs do not imply two radios or simultaneous arbitrary decoders.

**Acceptance:** closing a tab follows the declared application continuation policy; two tabs see the same selected QSO/automation state; unsupported combinations fail with an understandable explanation; no browser-side copy of a modem competes with the radio implementation.

## R8 — Simplify the browser connection surface

**Priority: early supporting work. Effort: medium.**

Consider serving the page, operating API, audio/control WebSockets and optional mirror under one HTTPS origin with named paths. The current remote constructs separate `:50002/:50003` and TCI `:50001/:50004` sockets (`views/remote.html:95-100`; `__main__.py:151-155`). One origin reduces extra certificate exceptions, firewall rules and special-case URL handling. Keep direct TCI available on its configured port for existing programs.

Offer a radio-displayed connect QR code containing the current local URL/pairing flow, alongside the existing mDNS and IP fallback. Explain the secure microphone setup in a short guided status view. Consider installable-app/PWA behavior only after certificate and secure-origin handling are reliable on the intended devices. Cached shell/settings pages can be useful offline, but they must never display cached RF state as current or queue RF commands offline.

Keep remote support local-first and avoid introducing a mandatory account, cloud dependency or analytics. Authentication credentials and pairing tokens should not be exposed in ordinary logs or a shareable screenshot URL.

**Acceptance:** after initial setup, one secure page loads the workspace and audio/control without independent port certificate surprises; direct TCI continues to work; an offline app launch says disconnected and leaves transmit unavailable.

## Phased roadmap

| Stage | Concrete scope | Exit condition |
|---|---|---|
| 0. Define the contract and benchmark | Capabilities, authoritative state, ownership/TX policy, failure semantics; measure present JPEG/TCI CPU, memory, traffic and delay on hardware | Agreed behavior for physical/local/TCI/browser conflicts and recorded baseline |
| 1. Useful native radio desk | Large DOM frequency/mode/filter/AGC/RIT/power/meters, readiness/source labels, RX audio, confirmed bounded PTT, reconnect; retain Mirror tab | Basic tune/listen/talk works from any native app screen; disconnect/permission tests pass |
| 2. Spots and log workflow | Side-by-side spots/memories, safe Tune, return history, idempotent log/correction, saved/pending states | A contact can be worked/logged without front-panel menu navigation; uncertainty produces no duplicate log |
| 3. Efficient waterfall and mobile/field | Structured or interim independent waterfall, adaptive rate, compact phone layout, Field Ops companion | Hardware load/network measurements improve or justify tradeoffs; portrait field tasks remain operable |
| 4. Application panels | One mode family at a time; CW queue before advanced digital workflows | Shared state and continuation rules remain consistent across radio and browser |
| 5. Optional wider-network features | Only if demanded: WAN audio transport, advanced session policy, offline draft sync | Explicitly tested threat/latency/recovery model with no regression to LAN or TCI |

Do not begin with a full SDR clone, arbitrary dashboard customization, all digital modes at once or WAN streaming. A small, direct, trustworthy operating desk plus the existing mirror would already be a substantial improvement.

## Validation checklist for a future implementation

These are acceptance ideas, not tests executed by this audit:

- **State:** physical knob, local menus, browser and TCI agree; unsupported features are absent/explained; pending commands never masquerade as applied state.
- **Hardware reality:** one RX/TX chain is honestly represented; independent browser panels/viewers share the radio; split/A/B appears only when implemented end to end.
- **Transmit:** busy/rejected/confirmed/releasing are distinguishable; known disconnect/crash cases have a bounded server-side outcome; no reconnect can automatically key RF.
- **Ownership:** view-only cannot control through either native panels or the mirror; revocation works on open sessions; the local operator can see and interrupt remote control according to policy.
- **Audio:** playback/microphone/source/transport failures are distinguishable; queues remain bounded; supported browsers and sample rates are tested; stale speech is discarded.
- **Logging:** durable acknowledgment, captured radio context and operation IDs prevent uncertainty from losing or duplicating a QSO; external uploads/spots have separate outcomes.
- **Performance:** compare idle, moving waterfall, active decoder and maximum supported viewers; measure radio CPU/memory/thermal behavior, traffic and control/audio latency; slow clients never stall the radio.
- **Accessibility:** core state is DOM text, labels/focus are complete, keyboard-only and screen reader paths work, 200% zoom and narrow phone layouts remain useful.
- **Compatibility:** the existing mirror, physical controls, TCI clients, settings, logs and recovery workflows remain available with documented policy changes.
