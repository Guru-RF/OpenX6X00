# wsprd (vendored)

WSPR decoder by K1JT and K9AN (WSJT-X `lib/wsprd`), as made into a library
by VA2GKA for rtlsdr-wsprd.

Source: https://github.com/Guenael/rtlsdr-wsprd
Commit: `1ca9b83dd2562ce9ef2453aacdd5bc3aab982c7d` (2026-03-05)
License: GPLv3 (see `LICENSE`)

Copied from that commit: `wsprd/*` and `LICENSE`.
`CMakeLists.txt` and this file are ours.

## Local patches

Each one is its own commit on top of the pristine import.

- `wsprd.c`: no FFTW wisdom file (it was read from and written to the
  working directory on every decode).
- `wsprd.c`: the coarse time search skips symbols before the start of the
  recording instead of reading before the start of the spectrum rows.
- `wsprd.c`: DT is 0 for a signal starting 1 s into the recording, as in
  WSJT-X (rtlsdr-wsprd subtracts 2 s for the delay of its own pipeline).
