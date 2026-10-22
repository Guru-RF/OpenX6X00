# Codex Possible Improvements

Reviewed 22 October 2026 at `d6bf443ac07cb7978a4c7499decea25d95a25c90`.

These are product and usability ideas, based on source, local documentation and screenshots in the repository. They are proposals, not changes or promises. No radio, physical controls, microphone, network connection or outdoor session was available for hands-on validation. The [code audit](Codex-Audit.md) covers correctness defects; the [remote document](Codex-Remote-Improvements.md) develops the browser operating experience. Source references abbreviated as `views/...` or `static/...` are under `webserver/src/x6100_webserver/`.

## What already works well

- The APP and Modes grids, Settings icon rail, MFK selection hints and fallback text menus already improve discovery. Preserve both the visible explanations and the hardware-first interaction. See `src/apps_menu.c:4-7`, `src/apps_menu.c:45-58`, `src/apps_menu.c:223-226`, `docs/MENUS.md:3-7`.
- Field Ops already has large program tiles, persistent activation state, offline reference lists, automatic re-spotting, immediate ADIF writes, detailed storage errors and activation progress. These are a strong base; a redesign should improve the journey rather than duplicate them. See `docs/MANUAL.md:1287-1294`, `1315-1327`, `1339-1359`, `1414-1436`; screenshot `docs/screenshots/field_home.png`.
- Themes, fonts, an Outdoor theme, a VISION theme, large value popups, S-meter alternatives and a portrait Walkie mode already exist. Accessibility work can extend these rather than merely proposing another theme. See `docs/MANUAL.md:286-336`, `388-409`, `1567-1605`, `docs/THEMES.md`.
- Browser configuration already handles structured edits, searchable logs, filtered ADIF/CSV export, backup/restore, gallery downloads, display settings and network naming. Preserve this investment and use it for preparation and administration. See `webserver/src/x6100_webserver/views/index.html`, `views/logs.html:5-19`, `views/backup.html`, `views/display.html`.
- Hardware knob acceleration, keyboard support, long-press customization, mDNS, Ethernet fallback and TCI already support experienced operators. The opportunity is making those capabilities understandable and predictable. See `docs/MANUAL.md:48-82`, `362-371`, `1727-1804`.

## Recommended priorities

Priority means suggested sequence, not severity of a bug. Effort is a relative estimate that needs design and hardware validation.

| ID | Idea | Main benefit | Priority | Effort |
|---|---|---|---|---|
| U01 | First-run and task readiness view | Faster first successful operation | High | Medium |
| U02 | One navigation and naming model | Less memorization and getting lost | High | Medium |
| U03 | Explain active control, operating state and TX source | Fewer surprising changes | High | Medium |
| U04 | Clear applied/saved/error states for every edit | Confidence that settings and logs are safe | High | Medium |
| U05 | Simpler field activation and logging workflow | Less effort during an activation | High | Medium |
| U06 | Accessibility beyond theme selection | Readable, operable controls for more people | High | Medium |
| U07 | Search, favorites and small operating profiles | Faster access for regular users | Medium | Medium |
| U08 | Persistent task and connection feedback | Easier recovery from failures | Medium | Medium |
| U09 | Better memory, frequency and text editing | Fewer entry mistakes | Medium | Medium |
| U10 | Explain digital-mode automation | Operator can see what happens next | Medium | Medium/Large |
| U11 | Backup, maintenance and troubleshooting journey | Easier support and recovery | Medium | Medium |
| U12 | Measure operator task success | Prioritize by observed friction | Cross-cutting | Small/Medium |

## U01 — First-run setup and readiness, organized around a task

**Current basis:** the manual's first steps ask for callsign, locator, network and accurate clock in different settings sections (`docs/MANUAL.md:33-46`). The web home page is a collection of configuration destinations (`views/index.html:1-40`).

**Idea:** offer a short, skippable setup route and then a compact station readiness card. Let the operator choose “Listen”, “Voice/CW”, “Digital modes” or “Portable activation”; show only the prerequisites of that task. A listening session should not require a callsign or internet. A digital session should show clock source, last synchronization and whether its quality is known. An activation should show position source and age, selected reference, storage writable, available space and relevant spot-service configuration.

Keep the actual operational status separate from configuration: “WiFi connected” does not mean “internet reachable”; a locator is not a fresh GPS fix; a clock displayed to the second does not establish its accuracy. When accuracy is unknown, say so rather than inventing a precision estimate. A “Fix this” action should take the operator directly to the relevant setting and return to the task.

**Acceptance:** a new operator completes one receive session and one chosen transmit workflow without consulting the manual for menu locations; offline operation remains available; each readiness failure has a concrete next action.

## U02 — Make navigation, names and exit behavior consistent

**Current basis:** APP has icon grids while underlying button pages still work; the manual describes APP page numbers and app-specific page counts (`docs/MENUS.md:3-7`; `docs/MANUAL.md:50-63`). Field Ops uses GEN/Radio to exit (`1284-1285`). General, Info, Wireless, TX Cal, Channels and MEM are terms the operator must map between multiple surfaces. `docs/MENUS.md` itself exists to explain this mapping.

**Idea:** use a canonical set of user-facing names in the radio, browser, manual and screenshots. Distinguish “Memory slots” and “Named channels” explicitly. Make the current section and page visible, with the same meaning for Back, Close and Radio. Keep the predictable APP/ESC exit path and show that path in contexts that use another physical key. Retain existing shortcuts so experienced users do not lose their muscle memory.

A short “Controls on this screen” overlay can explain MFK selection, main knob tuning, available F-key pages and configured long-press actions. Use contextual text rather than adding permanent clutter. New documentation should lead with current grids; legacy page paths can remain as a secondary route. Clarify README statements about removing the old remote versus introducing the current browser remote (`README.md`, “What changed from the fork” versus “Browser remote”).

**Acceptance:** the same task has the same label across surfaces; every full-screen app has a visible route back; a user can discover their current long-press bindings without entering the editor.

## U03 — Make state, control ownership and microphone source unmistakable

**Current basis:** the main screenshot shows many compact PRE/ATT/ATU/NR/NB/DNF and connectivity indicators. The active encoder context is displayed through button labels and optional popups (`docs/screenshots/main.png`; `docs/MANUAL.md:388-402`). The browser PTT uses the radio microphone unless Mic is enabled, then uses the browser microphone (`docs/MANUAL.md:1705-1711`). TCI clients can operate the same radio (`1727-1768`).

**Idea:** a consistent compact status strip should answer: Which VFO is active? Which frequency and mode will transmit? What will this knob change? Which microphone/audio path is selected? Is the radio receiving, awaiting a TX request, transmitting or blocked? Is a remote client controlling it? Expand an unfamiliar status into a short explanation when selected.

For audio, distinguish radio speaker volume, browser playback volume and TX microphone gain. Name sources as “Browser microphone”, “Hand microphone” or another actual selected input rather than a generic Mic toggle. Show the last hardware-confirmed state; an illuminated local button alone should not imply that RF transmission started. If a protection stops TX, keep its reason visible long enough to act on it.

Use text and shape as well as color. Do not hide the RX/TX state inside the screen mirror or a temporary toast. Future controller ownership must also cover TCI, CAT and local controls; see the remote ideas document.

**Acceptance:** in a test with local controls plus one browser plus a TCI client, an operator can always identify the active source and actual TX state. No identical “Volume” control affects a different destination without explanation.

## U04 — Treat edits, saving and deletion as an explicit workflow

**Current basis:** Display applies changes as they are made (`views/display.html:3`); CW messages appear on the radio the next time the relevant menu opens (`views/cw_messages.html:3`). Memory/channel/CW editors save one row and rebuild the returned table (`views/memories.html:82-113`; `views/cw_messages.html:33-41,64-67`). Logs have confirmation for destructive operations and block deleting the active activation (`views/logs.html:124-135`).

**Idea:** standardize “Unsaved”, “Saving”, “Saved”, “Applied on radio” and “Failed” where those distinctions matter. State whether the change is immediate, next-open or restart-required next to the relevant setting. Keep unsaved text and row edits when another row saves or a refresh occurs. Offer Discard and Save all where multiple rows are naturally edited together. When someone else changes the same setting, explain the conflict and offer reload/review instead of silently replacing the draft.

Use explicit Remove key actions for stored secrets. Today QRZ uses “empty = keep” while HOTA/WWFF use “empty = remove” on the same page (`views/field.html:13-21,43-70`); a consistent empty-field rule avoids surprises. Show actionable validation beside the field and retain the entered value after failure.

For destructive actions, display what will be lost and offer an undo/trash mechanism where practical. Logs already have stronger confirmation than memory/channel/CW row deletion; bring the simpler editors up to the same predictable standard without adding confirmation to routine reversible tuning.

**Acceptance:** editing three rows then saving one does not erase the other drafts; every setting states when it takes effect; delete/restore behavior is consistent; no API key is removed merely because the operator left a replacement field blank.

## U05 — Extend Field Ops into a complete activation journey

**Current basis:** Field Ops already supports offline lists, location fallback, saved activation state, automatic re-spots, immediate logging, QSO targets and QRZ upload. The browser Field Ops page currently centers on API keys, reference updates and log downloads (`views/field.html:1-112`).

**Ideas:**

1. **Prepare:** select program/reference and station profile, verify cached references and date, show position source/age/uncertainty, prepare CQ and check power/storage before leaving. Present the suggested nearby reference as a candidate to verify. A coarse locator or a phone elsewhere should not silently become the station location.
2. **Operate:** put CQ, Stop, Log QSO and current activation progress together. Show program/ref, callsign/profile, current RF state, count, last saved QSO and the latest confirmed spot. Use the existing CQ and auto-spot rules, not a parallel implementation.
3. **Log:** keep callsign entry ready, support hardware keyboard entry, allow immediate correction of the last QSO, and give a nonblocking duplicate hint. Distinguish “Saved on radio” from “Waiting in browser” and “Uploaded to service”. Auto-fill the frequency, mode, UTC and reference captured at log submission and let the operator inspect them.
4. **QSY:** show the proposed new frequency/mode and the re-spot delay/rate limit. Keep “listening elsewhere” distinct from “calling here”. Display each destination's actual success/failure rather than a single ambiguous Spot success message.
5. **Finish:** End activation summarizes counts, unresolved log writes/uploads, export location and QRT spot result. Allow a resume after accidental app exit or reboot, building on existing saved state.

Treat activation validity counts as guidance with visible rule/source/version information where maintained; they should not be presented as authoritative eligibility adjudication. Keep remote phone location explicitly opt-in and distinguish it from the radio's location.

**Acceptance:** an offline activation can start, log, recover after app reopen and export without internet. A repeated Save after uncertain delivery creates one QSO, not two. A failed spot leaves the locally saved QSO intact. A phone user can correct the last call without leaving the operating screen.

## U06 — Accessibility and outdoor use beyond choosing a theme

**Current basis:** fixed 800×480 native layouts, theme/font choices, Outdoor and VISION themes, big value popups and Field Ops tiles already provide options. The web pages use semantic inputs/buttons in places, but the remote's state is an image canvas and several action buttons use an icon/title only (`views/remote.html:57-62`; `views/memories.html:53-54`; `views/logs.html:41-43`). Dense tables scroll horizontally (`static/css/base.css:54-57`).

**Idea:** support a coherent large-text mode on the native display for key operating tasks, with fewer simultaneous controls rather than text enlargement that clips existing layouts. Keep frequency, RX/TX state, selected item and Back/Stop visible. Let users reduce ornamentation, choose high contrast, and keep a clear focus indicator in every theme. Give color-coded states an accompanying symbol or label.

For the browser, use real labels and accessible names for every field/action, a predictable tab order, visible keyboard focus, named regions and concise status announcements. Native DOM frequency/meter readouts should complement the waterfall canvas. Support 200% text zoom and narrow layouts. Offer optional spoken frequency/TX announcements and reduced motion, with a way to avoid constantly announcing meter updates.

For field use, test touch targets with gloves, sun glare, one-handed operation and accidental drags. Place Stop and PTT where both are reachable. Do not put frequent tasks behind a long press that has no visible equivalent. The remote should keep essential controls visible while the on-screen keyboard is open.

**Acceptance:** core browser operating tasks work with keyboard only, screen reader and touch; active RX/TX and errors remain distinguishable in grayscale; native large text has no clipped essential labels; an operator can log a call and stop a CQ in portrait mode without hunting through pages.

## U07 — Search, favorites and small profiles

**Current basis:** thirteen APP tiles plus thirteen digital modes, many settings cards and configurable long-press shortcuts (`src/apps_menu.c:45-58`; `docs/MENUS.md:32-52`).

**Idea:** add a compact Favorites row or startup task choice, retaining the existing full grid. On browser/keyboard, a command search can find “clock”, “microphone”, “FT8”, “POTA” or “screen brightness” and open the correct page. Include synonyms for physical panel abbreviations. Search should navigate to a transmit action, not execute it immediately.

Consider small, inspectable presets such as Home SSB, Field CW and Digital. Preview exactly which values change and exclude secrets/network access by default. Keep station identity, display preferences and RF operating presets conceptually separate, so a theme choice does not unexpectedly change TX power or microphone input. Reuse the existing theme and configuration mechanisms rather than adding a second settings store.

**Acceptance:** common tasks are reachable within two deliberate actions from the main screen/favorites; a profile preview makes every RF-affecting change visible before application.

## U08 — A persistent task and connection panel

**Current basis:** reference downloads already show progress and retries (`docs/MANUAL.md:1315-1324`); other operations report in separate per-page status areas. The browser remote shows fps/kB/s, sound state and transient messages (`views/remote.html:125-130,267-301`).

**Idea:** a small recent-activity panel should record “Reference list updated”, “QSO saved”, “Spot failed: offline”, “TX stopped: protection”, “Remote reconnected” and “Backup complete”. Each entry should show time, outcome and a useful next action. Preserve important errors until acknowledged and allow a concise copyable local diagnostic summary with secrets removed.

Separate page connection, radio control connection, audio playback, microphone readiness and actual TX state. Loading, empty, offline, disabled and failed should have different text. A stable success label should not overwrite an unresolved error every few seconds. Keep task feedback local; this does not require telemetry or automatic uploads.

**Acceptance:** after returning from another screen, the operator can tell whether their last save/spot/update completed and why a failed operation needs action.

## U09 — Frequency, memory and text entry suited to the task

**Current basis:** named channels and memory slots share table editors, while native mode frequency entry uses kHz and browser memory entry uses MHz (`views/memories.html:26,57-75`; `docs/MANUAL.md:779-783`). CW and digital text use macros and explicit queues.

**Idea:** accept explicit units consistently, display the resolved frequency before applying and preserve the radio's full supported resolution. The current precision-loss defect is documented as A13 in [the audit](Codex-Audit.md). Use digit-focused tuning for quick changes, plus explicit step controls. Show band/mode consequences when a new frequency changes them. Keep receive tuning available beyond a selected amateur band without disguising what is or is not permitted to transmit.

For channels, add search, tags/favorites and safe bulk import/export with a preview of validation errors. On mobile, edit one memory in a labeled card instead of a row with many tiny cells. For CW/message editing, preview macro substitution (`{CALL}`, `{HIS}` and mode-specific tokens), character limits and estimated duration before sending. Display original template and transmitted expansion separately.

**Acceptance:** a frequency entry means the same thing on radio and browser; editing a name does not change frequency precision; imported rows can be reviewed before replacement; a long macro cannot conceal what will actually be transmitted.

## U10 — Show what digital automation will do next

**Current basis:** FT8/FT4/FT2, JS8, WSPR and keyboard modes already have substantial automation, transmit queues, beacons and operating options (`docs/MANUAL.md:652-1043`). Some mode controls span many F-key pages (`58-59`, JS8 described as seven pages).

**Idea:** present a small state timeline: Listening → Decode → Reply queued → Next TX slot → Sending → Logged. Show the selected station, message, frequency/offset, upcoming slot and reason for waiting. Label automatic replies, heartbeat/beacon duty cycle and whether they continue after an app switch or remote disconnect. Separate a draft from a queued and an on-air message.

Provide one visible Stop/Cancel that explains whether it stops the current transmission, queued message, repeat CQ, beacon or all of the above. Support a receive-only/dry-run preview that never keys the transmitter. Expose structured decoded text to browser operating panels later, rather than making users read it from an 800×480 JPEG.

**Acceptance:** before enabling automation the operator can see the next message and the conditions under which it transmits; after Stop the UI clearly states which future transmissions remain armed.

## U11 — Maintenance and recovery as understandable tasks

**Current basis:** backup/restore, signed bundle updates and rollback, storage error explanations and network naming already exist (`docs/MANUAL.md:84-108,1344-1350,1609-1634,1770-1785`; `views/backup.html`, `views/update.html`).

**Idea:** show last successful backup, included parts and local backup destination; make Restore a preview of what will change. Give firmware, baseband and data format versions meaningful names, with a clear “update available / current / could not check” distinction. Offer local support bundles only when requested, with a preview of included data and automatic removal of secrets.

When network name/address changes, provide the next URL and a reconnect link. Make storage free/writable state visible before a recording or long activation. Explain recovery using normal operator language rather than shell commands. Preserve the existing manual fallback when the network fails.

**Acceptance:** an operator can verify a backup exists, understand a restore's scope, recover the browser after a network change and identify a full/read-only SD card without SSH.

## U12 — Validate with a short operator task study

No performance/usability measurements were taken in this review. Use a small test group covering new and experienced operators, field gloves, keyboard users and reduced vision. Time task completion, count wrong turns and record actual error recovery; keep this local and opt-in.

Suggested tasks: first receive session; set microphone and make a short controlled transmission; find an unfamiliar setting; tune and save a channel; start an offline POTA activation; log and correct a QSO; stop automatic CQ; distinguish a failed upload from a failed local save; recover after WiFi loss; export and restore a backup. Establish a baseline before choosing numerical targets.

## Suggested delivery order

1. **Language and feedback:** canonical labels, current docs, source-specific audio labels, applied/saved/error messages, accessible field/action names and visible exit/stop routes.
2. **Core journeys:** readiness view, draft-safe editors, field log/correction workflow, mobile cards and consistent navigation.
3. **Power-user speed:** favorites, search, inspected profiles, macro preview and digital automation state.
4. **Validate and refine:** compare the same tasks before/after on hardware and phones; remove additions that add clutter without improving success.

The largest usability return is likely to come from showing state and completing tasks with fewer context switches, while preserving the reliable physical knob and key interactions already present.
