# mbelib (vendored, decoder only)

The AMBE+2 "half rate" voice decoder (3600 bit/s with FEC, 2450 bit/s of voice) that DMR uses, from mbelib 1.3.0.
The DMR app (src/dialog_dmr.c, src/dmr_ambe.c) turns the talkgroups' voice into sound with it. The radio only
decodes: there is no encoder here, and nothing in the GUI sends voice to a DMR network.

Source: https://github.com/szechyjs/mbelib
Commit: `9a04ed5c78176a9965f3d43f7aa1b1f5330e771f` (2019-05-28)
License: ISC (see `COPYRIGHT`, identical to upstream's)

Copied from that commit, unchanged (each file's git blob is the one of that commit):
`ambe3600x2450.c`, `ambe3600x2450_const.h`, `ecc.c`, `ecc_const.h`, `mbelib.c`, `mbelib.h`, `mbelib_const.h` and
`COPYRIGHT`. Left out: the other codecs (P25's IMBE 7200x4400, 7100x4400, and the D-STAR AMBE 3600x2400 file), the
tests, the packaging. `mbelib.h` still declares their functions; nothing calls them, so nothing of them is linked.
`CMakeLists.txt` and this file are ours.

mbelib is an independent decoder, written by an unnamed author from the published P25 half-rate vocoder standard
(TIA-102.BABA-1) and DVSI's patents, as its code comments and the accounts of it say; it is not DVSI's code.
Upstream's README asks readers to check for patent restrictions before using it. The patents on AMBE+2's half-rate
family (EP1465158B1, EP1748425B1) expired in Europe on 2024-03-26; US 8,359,197 may apply in the United States until
2028-05-20. The radio decodes only, it never encodes.

Notes for callers:
- `mbe_checkGolayBlock` (ecc.c) keeps its work in static variables, so one thread at a time decodes. The GUI has one
  DMR decoder thread.
- `mbe_decodeAmbe2450Parms` (ambe3600x2450.c, the two `prev_mp->log2Ml[intkl[l] + 1]`) reads one float past
  `log2Ml[56]` when the last frame had the most harmonics, L = 56: still inside the same `mbe_parms` (it is
  `PHIl[0]`), and weighted by `deltal[l]`, which is 0 there (or about 1e-6 from rounding). Harmless as it is, and
  left unchanged so the files stay upstream's; tools/dmr builds mbelib without UBSan's bounds check for that reason,
  with ASan on.
