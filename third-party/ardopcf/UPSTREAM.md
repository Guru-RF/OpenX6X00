# ardopcf (vendored)

ARDOP HF modem (TNC) by Rick Muething KN6KB, John Wiseman G8BPQ and Peter
LaRue, as ardopcf 1.0.4.1.3. Pat talks to it on its host ports for Winlink.

Source: https://github.com/pflarue/ardop
Commit: `e27af046c04f973ed8958a219844f655ead6c419` (2026-09-07)
License: MIT (see `LICENSE`), except the libraries under `lib/`, below

Copied from that commit: `src/`, `lib/`, `webgui/`, `LICENSE`, `changelog.md`,
`Makefile` and `README.md` (not `docs/`, `test/` or `host/`).
`UPSTREAM.md` is ours.

Upstream development has stopped (README.md). The README asks that anything
released from ardopcf be given a new name, and that a modem which is not
over-the-air compatible not use "ardop" in its name; ours stays compatible.

## Licences

- ardopcf itself (`src/`, `webgui/`): MIT, Rick Muething, John Wiseman,
  Peter LaRue (`LICENSE`). It does not cover `lib/`; each library there names
  its own author and licence.
- `lib/ws_server` and `lib/txt2c`: MIT, Peter LaRue (in the file headers).
- `lib/zf_log`: MIT, wonder-mice (`lib/zf_log/LICENSE`).
- `lib/rockliff/rrs.c` (the Reed-Solomon coder): Simon Rockliff's own notice,
  in `lib/rockliff/rrs.txt`. It asks that his name stays in the comments at
  the head of the program:

  > This program may be freely modified and/or given to whoever wants it.
  > A condition of such distribution is that the author's contribution be
  > acknowledged by his name being left in the comments heading the program,
  > however no responsibility is accepted for any financial or other loss
  > which may result from some unforseen errors or malfunctioning of the
  > program during use.
  > Simon Rockliff, 26th June 1991

- `lib/hid` (HIDAPI: GPLv3, BSD-style or its original licence, at the user's
  choice) and the PJRC Raw HID part of `lib/rawhid/rawhid.c` (MIT-style):
  Windows only, not in the Linux build. `lib/hid/LICENSE.txt` names
  `LICENSE-gpl3.txt` and `LICENSE-bsd.txt`, which upstream does not ship.
- The tiny_gpio code in `src/linux/ALSASound.c` (Raspberry Pi GPIO PTT, ARM
  builds): public domain.

## Local patches

Each one is its own commit on top of the pristine import.

- `src/linux/SockSound.c` (new) and hooks in `src/linux/ALSASound.c`: the
  capture device `SOCK:<path>` takes the audio and the PTT through the GUI's
  UNIX socket (protocol at the top of `SockSound.c`) instead of ALSA, which
  is then never opened. `Makefile` builds it too.
