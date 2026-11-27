# Waterfall 3D: smoother look, noise kept, close-up strip — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The 3D waterfall shows the noise as a low smooth base with rounded signal ridges and some polish, and (X6100 only) an optional close-up strip on top: a fine 2D waterfall of 2/5/10 kHz around the VFO with an "off by" label.

**Architecture:** Two pure C modules carry the logic that can be tested on the PC without LVGL: `wf3d_shape` (a row's levels → trace heights: smoothing, noise base, peaks kept) and `closeup_est` (the offset of the strongest signal in the passband). The close-up gets its own narrow spectrum in `dsp.cpp` (mix the IQ to the VFO, decimate, 256-point FFT) and its own view `closeup_view.c`, which takes the top of the waterfall area; `waterfall_view.c` draws the 3D in the rest.

**Tech Stack:** C (gnu11) and C++ (dsp.cpp), LVGL 8, liquid-dsp (firdecim, nco, fft), host tests as `tools/<name>/Makefile` + `*_test.c` (pattern: `tools/smeter_att`), the PC build (`host/build.sh`, Linux; here in the Docker container that mounts the repo at `/w`), `tools/screenshots/host_shots.py`, `host/_measure.sh`, callgrind.

**Spec:** `docs/superpowers/specs/2026-11-27-waterfall-3d-closeup-design.md`

## Global Constraints

- Colours stay by strength; the three *3D colours* choices (Strength, Waterfall, Blue) all still work.
- The noise base stays low: a faint CW signal (10 dB above the noise) must stand clearly above it.
- A narrow CW signal keeps its height through the smoothing (#161's reason for keeping peaks).
- Close-up setting: `wf_closeup`, choices **Off, 2 kHz, 5 kHz, 10 kHz** (values 0..3), default **5 kHz** (2).
- Close-up only with 3D on; height `height / 4` (fixed); **hidden on HW_CAP_PSD bases** (X6200): settings window, webserver, cfg_socket.
- 3D keeps its cap of 30 pictures a second (`D3_MIN_MS`) and the UI does not lag. If short: drop glow, then translucency, then default depth. No run-time load switching.
- "See-through" = translucent tint with lighter edges; traces behind do not show through (the hidden-part skip of #161 stays).
- Commit messages in the repo's style ("Waterfall 3D: …"), ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Comments and the manual in the repo's existing prose style.

## Review Focus

1. **Tuning fast / changing band while the close-up shows** — the strip clears and restarts at the new frequency; no smear of the old one, no stale "off by". (Task 4: host shot after a band change; Task 5 test `reset_on_retune`.)
2. **No signal in the passband** — the label hides, it does not show a random offset from noise. (Task 5 test `noise_only_no_offset`.)
3. **Transmit** — the close-up stops updating (TX IQ is not the received band) and the label hides; the strip comes back on RX. (Task 4 step.)
4. **Mode changes (LSB/USB/CW/AM/digital)** — the passband on the strip and the "want" frequency of the estimate follow the mode (CW: the pitch; SSB: the filter's middle). (Task 5 test `cw_pitch_target`, `lsb_mirror`.)
5. **Settings applied live** — switching 3D off, Close-up Off, or 2→10 kHz while running: the area resizes at once, no restart. (Task 4 host shot script.)

---

### Task 1: The Close-up setting (hidden on the X6200)

**Files:**
- Modify: `src/cfg/settings_manager.h` (after `p_wf_3d_glide`, ~line 301)
- Modify: `src/cfg/cfg_api.h`, `src/cfg/cfg_api.cpp` (next to `cfg_wf_3d_range`)
- Modify: `src/cfg_socket.cpp:58-65`
- Modify: `src/settings_window.cpp:3162-3177` (`make_wf_3d()`)
- Modify: `webserver/src/x6100_webserver/views/display.html` (next to the 3D settings)
- Modify: `docs/MANUAL.md:717-730` (the 3D table and text)

**Interfaces:**
- Produces: `Parameter<int32_t> p_wf_closeup{"wf_closeup", 2, 0, 3, …}`; `extern ParamInt *cfg_wf_closeup;`; `uint32_t wf_closeup_span_hz(int32_t v)` in `src/cfg/cfg_api.h` returning 0, 2000, 5000, 10000.

- [ ] **Step 1:** Add the parameter, the `cfg_wf_closeup` pointer and `wf_closeup_span_hz()`, following `p_wf_3d_range` exactly.
- [ ] **Step 2:** In `make_wf_3d()`, under the 3D switch while 3D is on and `!(hw_caps() & HW_CAP_PSD)`: `make_param_choice(row, "Close-up", " Off \n 2 kHz \n 5 kHz \n 10 kHz ", cfg_sm.p_wf_closeup)`.
- [ ] **Step 3:** `cfg_socket.cpp`: `{"wf_closeup", &cfg_sm.p_wf_closeup, 0, 3}` — skipped (not listed / rejected) on HW_CAP_PSD, the way the socket already treats a base-specific setting if there is one; otherwise a check at the entry. Webserver: the choice, hidden when the radio reports PSD (use what the page already knows about the base; if it knows nothing, hide on the `wf_closeup` key missing from the socket's list).
- [ ] **Step 4:** Manual: a "Close-up" row in the table (Off, 2, 5, 10 kHz across, 5 by default; X6100 only) and two sentences on the strip and the "off by" label; *3D floor* now "how high the noise base sits".
- [ ] **Step 5: Build** — `host/build.sh build` in the container. Expected: builds without new warnings.
- [ ] **Step 6: Shot** — `python3 tools/screenshots/host_shots.py build-host "key GEN; …; shot settings_3d" /out --set wf_3d=1` (navigate to Display > WATERFALL as earlier 3D shots did). Expected: "Close-up" with 5 kHz selected under the 3D rows.
- [ ] **Step 7: Commit** — "Waterfall 3D: the Close-up setting, Off, 2, 5 or 10 kHz (not on the X6200)".

### Task 2: The noise kept as a low smooth base (`wf3d_shape`)

**Files:**
- Create: `src/wf3d_shape.h`, `src/wf3d_shape.c`
- Create: `tools/wf3d/Makefile`, `tools/wf3d/wf3d_shape_test.c` (pattern: `tools/smeter_att`)
- Modify: `src/waterfall_view.c` — `d3_levels()` (~926), `d3_cut()` (~979), `d3_levels_of()` (~994), `d3_render()` (~1082), the "3D" comment block (~515)
- Modify: `src/cfg/settings_manager.h` (`p_wf_3d_floor`: meaning, default 12, range 0..40 kept), `src/settings_window.cpp:3147` (label stays "3D floor", format "%i")
- Modify: `CMakeLists.txt` / `src` sources list if files are listed explicitly

**Interfaces:**
- Produces:
  ```c
  /* Levels (0..255, the waterfall's colour indexes) of one row, n pixels, to trace heights 0..255:
   * smoothed (7-tap binomial 1 6 15 20 15 6 1 / 64); the noise (at or below floor) pressed into 0..base;
   * above the floor base + (v - floor) * scale / 256, clamped to 255; where the raw level stands more than
   * WF3D_SIG levels above the floor the height is at least that of the raw level's 3-tap mean (peaks kept). */
  #define WF3D_SIG 6
  void wf3d_shape(const uint8_t *raw, int n, uint8_t floor, int32_t base, int32_t scale, uint8_t *out);
  ```
- Consumes: `row->floor`, `d3_scale()`, `param_i_get(cfg_wf_3d_floor)` as `base` (0..40 → height levels base*2, so default 12 → 24 of 255: about a tenth of the height).

- [ ] **Step 1: Write the failing tests** in `tools/wf3d/wf3d_shape_test.c` (same `check()` helper as smeter_att), n = 800, floor = 100, base = 24, scale = 256:
  - `flat_noise_is_smooth`: raw alternating 92/108 → every out in [base-6, base+6] and max−min of out ≤ 6.
  - `noise_below_base`: raw all 60 → every out ≤ 24 and ≥ 0.
  - `floor_maps_to_base`: raw all 100 → every out == 24 (±1).
  - `cw_spike_kept`: raw 100 everywhere, raw[400] = 160 → out[400] ≥ 24 + 0.9*60*(256/256) - 1 (≥ 77).
  - `faint_cw_above_noise`: raw alternating 92/108, raw[399..401] = 120 → out[400] ≥ max(out of the noise) + 10.
  - `clamp_top`: raw all 255, scale 1024 → all out == 255.
- [ ] **Step 2: Run** `make -C tools/wf3d` — Expected: fails to build (no `wf3d_shape.c`).
- [ ] **Step 3: Implement** `wf3d_shape()` in `src/wf3d_shape.c` (no LVGL includes: it builds in the test alone).
- [ ] **Step 4: Run** `make -C tools/wf3d` — Expected: all checks pass, `0 failures`.
- [ ] **Step 5: Use it in the 3D:** `d3_levels()` gets the raw row (`row_pixels()`) and `d3_levels_of()` calls `wf3d_shape(raw, WIDTH, row->floor, floor_setting * 2, scale, lv)`; the cache key `cut` becomes `base`. `d3_cut()`'s floor cut goes. The `blk[]` empty-block skip: every block now has the base, so mark a block "flat" when all its heights are ≤ base + 2 and draw such blocks in `d3_trace()` as a cheap flat strip (one fill per column at the trace's foot − base height, the base's colour, no per-pixel shading) — the full ridge drawing only for other blocks.
- [ ] **Step 6: Build and shot** — host build; `HOST_STUB_IQ=<the recording used for #161> python3 tools/screenshots/host_shots.py build-host "sleep 8; shot d3_base" /out --set wf_3d=1`. Expected: the whole band covered by a low textured base, signals standing above it, smooth ridges (compare with `.superpowers/…/look.html` B).
- [ ] **Step 7: Measure** — callgrind on the PC build as in commit 56be9c7 (instructions a picture). Record before/after in the commit message. Expected: ≤ 2.5M a picture (it was 1.8M); if more, make the flat-block path cheaper before going on.
- [ ] **Step 8: Commit** — "Waterfall 3D: the noise kept as a low smooth base, signals rounded, peaks still at their height" (+ the numbers).

### Task 3: Polish (glow, fading grid, tint)

**Files:**
- Modify: `src/waterfall_view.c` — `d3_background()` (~1018), `d3_grid()` (~754), `d3_trace()` (~792), the newest trace's line in `d3_render()` (end)

**Interfaces:** none new; constants `D3_GLOW_PX 3`, `D3_GLOW_OP 0.35f`, `D3_TINT 0.70f` (ridge body opacity over the background), `D3_EDGE_LIGHT` (the existing `d3_light()` on the top 2 px).

- [ ] **Step 1:** Newest trace: before its 2 px line, a glow — the same path at widths `1 + 2*D3_GLOW_PX` and `1 + D3_GLOW_PX` with opacity `D3_GLOW_OP` and `D3_GLOW_OP*1.5`, its own colour (`d3_join()` with wider `th`).
- [ ] **Step 2:** Grid and frequency lines in `d3_background()`: opacity falling linearly from 1 at the front to 0.15 at the back (drawn once into the background buffer, so no per-picture cost).
- [ ] **Step 3:** Ridge bodies: blend at `D3_TINT` over the background (`d3_blend()`), top 2 px lighter (`d3_light()`) at full opacity. Mono themes: unchanged (ink/paper as now).
- [ ] **Step 4: Build, shot, measure** — as Task 2 steps 6–7, with `--set wf_3d_colors=0`, `=1`, `=2` and a mono theme (`--theme` of a 1-bit theme, see `tools/screenshots/check_1bit.py`). Expected: the three colour choices and mono look right; instructions a picture ≤ 1.15 × Task 2's. If over: drop glow first (spec order).
- [ ] **Step 5: Commit** — "Waterfall 3D: polish, a glow on the newest trace, the grid fading into the distance, the ridges tinted".

### Task 4: The close-up strip (X6100)

**Files:**
- Create: `src/closeup_view.h`, `src/closeup_view.c`
- Modify: `src/dsp.cpp` (`feed()` ~235, `dsp_samples()` ~377, init), `src/dsp.h`
- Modify: `src/waterfall_view.c` — `waterfall_set_height()` (~1693), `refresh_waterfall()` (~1301), `d3_render()` (the 3D's height and top), `on_view_changed`
- Modify: `src/screen_main.c` (create the strip with the waterfall) if objects are made there

**Interfaces:**
- Produces (dsp → view, called on the DSP thread like `waterfall_data()`; the view copies into a ring under the same rules as the waterfall's rows):
  ```c
  /* One close-up row: n dB bins, lowest frequency first, over span_hz centred on center_hz */
  void closeup_data(const float *db, uint16_t n, uint32_t span_hz, uint32_t center_hz);
  void closeup_reset(void);                 /* retune, band change, span change, TX */
  lv_coord_t closeup_height(lv_coord_t wf_height);  /* 0 when not shown: 3D off, Off, HW_CAP_PSD */
  void closeup_init(lv_obj_t *parent);      /* the strip's object, top of the waterfall area */
  ```
- In `dsp.cpp`: a `Closeup` class (nco mix to `vfo − iq_centre`, `firdecim_crcf` by D, `Panorama(256)` with 50 % overlap). D per span: 2 kHz → 40 (2.5 kS/s, 9.8 Hz a bin), 5 kHz → 16 (6.25 kS/s, 24 Hz), 10 kHz → 8 (12.5 kS/s, 49 Hz). Shown bins: the middle `span/Fs` of the 256. Runs only while `closeup_height() > 0`, RX, and the waterfall seen. VFO from the same source `waterfall_view.c` uses (`cfg_fg_freq`, with `mode_lo_offset`/IF shift as in `on_fg_freq_change`).
- Consumes: `cfg_wf_closeup`, `wf_closeup_span_hz()` (Task 1).

- [ ] **Step 1: DSP** — the `Closeup` class and its call in `feed()` (on the undecimated `iq`); `closeup_reset()` on retune (where `wf_rx->reset()` is), span change, TX.
- [ ] **Step 2: View** — the strip: a scrolling 2D waterfall (newest row on top, the waterfall's palette and level scale `waterfall_get_levels()`), the centre line (the VFO line's colour), the passband edges and a light shade of it (from `line_passband()`'s numbers, mapped to the strip's span), "±N kHz" small at the left edge. Mono: ink/paper like the 2D waterfall.
- [ ] **Step 3: Split** — `waterfall_view.c` draws the 3D in `height − closeup_height(height)` below the strip; setting changes resize live (subscribe `cfg_wf_closeup` like `cfg_wf_3d`).
- [ ] **Step 4: Build and shots** with the recording: `--set wf_3d=1 --set wf_closeup=2` → strip on top, 3D below; `wf_closeup=0` → full-height 3D; `wf_closeup=1` and `=3`; a band change (`key PageUp; sleep 2; shot`) → strip restarted clean; PTT (`Ctrl+P`) → strip frozen. Expected: as described; a carrier in the recording shows as a sharp line ≤ 3 px wide at 2 kHz.
- [ ] **Step 5: Measure** — `host/_measure.sh` in the container, with and without the strip. Expected: GUI CPU with the strip ≤ +5 points over without; DSP time per packet in budget (no dropped packets in the log).
- [ ] **Step 6: Commit** — "Waterfall 3D: the close-up, a fine waterfall of 2, 5 or 10 kHz around the VFO above the 3D".

### Task 5: The "off by" label

**Files:**
- Create: `src/closeup_est.h`, `src/closeup_est.c`
- Create: `tools/wf3d/closeup_est_test.c` (same Makefile, second target)
- Modify: `src/closeup_view.c` (the label)

**Interfaces:**
- Produces:
  ```c
  typedef struct { float off_hz; bool valid; uint8_t quiet; } closeup_est_t;
  /* db: n bins over span_hz centred on the VFO. pb_lo/pb_hi: the passband in Hz from the VFO; want: where the
   * signal's centre should be, in Hz from the VFO (CW: the pitch with its sign; SSB: the middle of the
   * passband; AM: 0). The strongest signal: the energy centroid of the bins within 6 dB of the passband's peak,
   * when that peak is at least 10 dB over the strip's median. off_hz smoothed (0.3 new, 0.7 old); valid false
   * after 5 rows without a signal. */
  void closeup_est_update(closeup_est_t *e, const float *db, int n, uint32_t span_hz,
                          int32_t pb_lo, int32_t pb_hi, int32_t want);
  void closeup_est_reset(closeup_est_t *e);
  #define CLOSEUP_GREEN_HZ 20
  ```
- Consumes: Task 4's rows and passband numbers.

- [ ] **Step 1: Write the failing tests** (n = 256 over 2000 Hz, noise −100 dB with ±2 dB jitter):
  - `carrier_offset`: a carrier at −150 Hz, passband −1000..1000, want 0 → after 10 updates `valid` and `off_hz` within ±10 of −150.
  - `noise_only_no_offset`: noise only, 10 updates → `valid == false`.
  - `cw_pitch_target`: carrier at +600, want +600 → `off_hz` within ±10 of 0.
  - `lsb_mirror`: SSB-like block −2700..−300 shifted by +150, passband −2700..−300, want −1500 → `off_hz` within ±60 of +150.
  - `reset_on_retune`: after a carrier at −150, `closeup_est_reset()` then noise → `valid == false` and no carry-over.
  - `outside_passband_ignored`: strong carrier at +900, passband −500..500 with a weaker one at −100 → offset ≈ −100.
- [ ] **Step 2: Run** `make -C tools/wf3d` — Expected: fails (no `closeup_est.c`).
- [ ] **Step 3: Implement** `closeup_est_update()` / `closeup_est_reset()` (no LVGL).
- [ ] **Step 4: Run** `make -C tools/wf3d` — Expected: `0 failures`.
- [ ] **Step 5: Label** — in `closeup_view.c`: "off −150 Hz" right of the centre line, small, dark backing; amber, green when `|off| ≤ CLOSEUP_GREEN_HZ` ("centred"); hidden when not `valid` or TX. `want`/passband per mode from what `line_passband()` and the CW pitch already give.
- [ ] **Step 6: Shot** — tuned 150 Hz off a carrier in the recording → "off −150 Hz" amber; tuned onto it → green.
- [ ] **Step 7: Commit** — "Waterfall 3D: the close-up's off-by label, how far the strongest signal in the passband is from centre".

### Task 6: Whole-feature check and the pictures for the user

**Files:** `docs/MANUAL.md` (final wording), screenshots out of the repo (scratchpad).

- [ ] **Step 1:** `make -C tools/wf3d` all pass; host build clean; `host/_measure.sh` numbers for 3D alone, 3D + close-up, vs. the #161 numbers.
- [ ] **Step 2:** Shots for the user: the split at 2/5/10 kHz, close-up Off, the three colour choices, mono — from the recording.
- [ ] **Step 3:** Review Focus walk-through (the five lines above) on the host build, each with a shot or a log line.
- [ ] **Step 4: Commit** the manual if changed.
