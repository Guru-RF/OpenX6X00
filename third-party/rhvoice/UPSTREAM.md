# RHVoice (the C interface's two headers only)

The speech synthesizer behind Settings > Voice. Nothing of RHVoice is compiled or linked here: `src/speech_engine.c`
opens `libRHVoice.so.5` of the card image when speech is first asked for (dlopen) and calls it through the C
interface these two headers declare. A card image without the library has no speech, and the GUI runs the same.

Source: https://github.com/RHVoice/RHVoice
Version: 1.14.0 (the X6100 tree's package), commit `ed7bf65b6861281b2a0f168a148200f870036164` (the X6200 tree's)
License: LGPL-2.1-or-later (the notice at the top of each file)

Copied unchanged from `src/include/`: `RHVoice.h`, `RHVoice_common.h`.

What the card image carries is built by the buildroot package `rhvoice` of the radios' trees
(`buildroot/local-build.sh` selects it):

- `libRHVoice.so.5` and `libRHVoice_core.so.10`: LGPL-2.1-or-later. The build has no MAGE (GPL-3), which RHVoice's
  `doc/en/License.md` names as what would make the combination GPL-3.
- `libRHVoice_audio` (GPL-2-or-later) and the `RHVoice-test` program are not used and are removed from the image:
  speech_engine.c plays the samples itself, on PulseAudio.
- The English language data and the voices SLT, Alan, BDL and CLB. SLT's own
  README states "the CMU license"; `doc/en/License.md` lists no restriction for any of the four.
- Not in the image: Evgeniy and Lyubov, RHVoice Lab's voices under CC BY-NC-ND 4.0, whose use in a product needs
  the lab's and the speaker's permission.
