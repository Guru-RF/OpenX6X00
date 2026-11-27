# Waterfall 3D: a smoother look with the noise kept, and a close-up strip for tuning

Date: 2026-11-27. Branch: `feature/3d-scope`. Follows the 3D waterfall of #161 (`src/waterfall_view.c`, "3D").

## Why

The user compared our 3D waterfall with a reference picture of another SDR display (a "3D hologram",
newest in front) and wants ours closer to it:

- **Ours looks empty.** Each trace has its noise floor cut away (`d3_cut()`, the row's 75th percentile plus
  *3D floor*), so a band full of noise shows as a black floor with a few spikes. The user said: "it looks like
  we have no signal... but in fact there is a lot of noise." They want the noise base visible, "bumped up".
- **Ours looks jagged.** `d3_levels()` blends 5 pixels but keeps the raw value wherever it is higher
  (`LV_MAX(c, ...)`), so every noise spike survives. The reference has soft, rounded ridges.
- **Colours stay coded by strength.** The user said they want that kept, for sure.
- **A split screen, as an option:** a zoomed-in 2D strip above the 3D. Its purpose is fine tuning: to show
  at a glance whether you are centred on a signal ("fine scroll 2D type of thing"). Users can turn it off.
- **Polish the UI a bit.**

The user picked the look from the mockups made during the design (kept locally in
`.superpowers/brainstorm/…/content/look.html` option B and `split.html` option B; that folder is git-ignored).

## Goals

1. **3D look (mockup B):** the noise kept as a low, textured base across the whole band; signals rise out of
   it with smooth, rounded ridges; colour by strength as today (all three *3D colours* choices still work).
2. **Close-up strip (mockup split B):** with 3D on, an optional strip at the top of the waterfall area:
   - a 2D scrolling waterfall of a narrow span **centred on the tuned frequency**, following it as you tune;
   - the centre (VFO) line and the passband edges marked on it, the passband lightly shaded;
   - a small **"off by" label** near the centre line: the estimated offset of the strongest signal in the
     passband from where it should be (for example "off −150 Hz"), turning green when within a small margin.
3. **Settings:** on Settings > Display > WATERFALL, under the 3D switch with the other 3D settings, a
   **Close-up** choice: **Off, 2 kHz, 5 kHz, 10 kHz** (the span across the strip). Default **5 kHz**.
   Off gives the whole area back to the 3D.
4. **Polish of the 3D** (from the reference picture):
   - a soft glow on the newest trace in front, so "now" stands out;
   - the floor grid fading with distance, not hard lines;
   - ridges slightly see-through with lighter edges (the "hologram" feel), colours still by strength.

## Non-goals

- No change to the 2D waterfall or to the spectrum above it.
- No close-up with 3D off (the strip belongs to the 3D view).
- The strip's height is not a setting.
- No automatic tuning to the signal (the label only tells; the user tunes).

## Anti-goals (it fails even if it works)

- The noise base so high that weak signals (a faint CW trace) are lost in it. The base stays low; signals
  must still stand clearly above it.
- Smoothing that flattens real signals: a narrow CW signal must still stand at its height (the reason
  #161 kept peaks). Smooth the noise, not the signals away.
- A close-up too coarse to show a 50–150 Hz offset (see the constraint below).
- The radio's UI getting sluggish. If CPU runs short, the 3D gets simpler first, not the close-up.

## Decided

By the user:
- Look B (noise kept low, smooth, colour by strength).
- The split: zoomed 2D strip on top, 3D below; optional; with the "off by" label (split option B).
- The setting chooses the **span in kHz** of the strip, and Off.
- Polish the UI a bit.

Claude's calls (the user saw them listed and said to proceed):
- Spans Off / 2 / 5 / 10 kHz, default 5 kHz.
- Strip height about a quarter of the waterfall area, fixed.
- Strip only with 3D on.
- The "off by" estimate from the strongest signal in the passband: exact for CW and carriers, a rough guide
  for SSB (voice has no clear centre). Green within a small margin.
- *3D floor* changes meaning: how high the noise base sits, no longer how much is cut away. Its range and
  default may change; its manual text with it.
- If CPU is short, the 3D gives way first. This is a rule for building, not a feature: no automatic load
  measure or switching at run time. The builder picks the defaults (kernel size, effects, depth) by measuring
  on the radio, so the 3D keeps its 30 pictures a second while tuning (D3_MIN_MS) and the UI does not lag.
  What gives way first, in order: glow, then translucency, then smaller default depth; glide stays (it has
  its own switch).
- "Ridges slightly see-through" means a translucent tint with lighter edges inside the current renderer:
  traces behind do **not** show through those in front. The hidden-part skip of #161 stays.
- Keeping the noise base loses the skip of empty floor (`blk[]` in `d3_trace()`); the builder finds a
  cheaper way to draw the low base (it is low and smooth) rather than drawing every column as a full ridge.

## Constraint: the close-up needs its own resolution

The waterfall rows are a 1024-bin FFT over the whole IQ span (`wf_span()` in `src/dsp.cpp`: 100 kHz at zoom 1,
about 98 Hz a bin). A 5 kHz strip from them would be ~50 bins wide, a 2 kHz one ~20: useless for centring.
The close-up needs its own narrow spectrum around the VFO: mix the IQ to the VFO, decimate to the strip's
span, and take its own FFT (as `feed()` already decimates for the spectrum's zoom). Aim for about 10 Hz a bin
or finer at 2 kHz, and a row rate like the waterfall's. This is new work on the DSP thread: keep it cheap
(only while the strip is shown, 3D on and the waterfall seen).

## The X6200: no close-up (decided by the user)

The X6200 base sends no IQ, only its own spectrum (`dsp_psd()`, HW_CAP_PSD): 512 bins over 384 kHz / 2^step,
used to step 3 (48 kHz, ~94 Hz a bin); what it does beyond is untested (`PSD_MAX_DEC`), and it is one stream
shared with the 3D and the waterfall. So on bases with HW_CAP_PSD the Close-up setting is **hidden**
(settings window, webserver display page, cfg_api) and the strip never shows; the new 3D look and polish work
there as on the X6100. Follow-up, not in this work: try narrower steps on a real X6200, or a strip from the
received audio.

## Left to the builder

- The smoothing kernel and how the noise base is drawn (height, translucency), within the anti-goals; tune it
  against a recording (`HOST_STUB_IQ` on the host build) and on the radio.
- How the "off by" estimate works (peak, centroid of the energy in the passband, per mode), its margin for
  green, and smoothing so the number does not flicker.
- How the passband maps on the strip per mode (USB, LSB, CW, digital), using what `line_passband()` already
  knows.
- The DSP side of the close-up: where in `dsp.cpp`, decimator and FFT sizes, how rows reach
  `waterfall_view.c`.
- The strip's colours: the waterfall's own palette.
- Mono (1-bit) themes: something sensible, as the 3D already does.
- Whether to add a quick-button action for the close-up (optional; "WF 3D on/off" is the pattern).
- The webserver's display page (`webserver/.../display.html`) and `cfg_api`/`cfg_socket` get the new setting
  as the other 3D settings did; `docs/MANUAL.md` describes it.
- Screenshots on the host build for the user to see.
