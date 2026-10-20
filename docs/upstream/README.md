# Upstream watch

One report per release of the closed X6100 firmware this project was forked from, made by
`tools/upstream-watch.py`. Each report says what the release publishes as source (its
buildroot tree and the modem helpers, diffed against ours, with the patches in the tool's
cache), what its GUI binary says in its own words (the interface strings ours does not have:
the features to look at), and which scripts, services and web pages its image has.

```
python3 -I tools/upstream-watch.py            # the latest release
python3 -I tools/upstream-watch.py v0.129     # one release
```

Needs `debugfs` (e2fsprogs), `strings` (binutils) and `diff`; downloads go to
`~/.cache/openx6x00-upstream/<tag>/` (about 110 MB a release) with the patches beside them.
With a buildroot build next door (`../AetherX6100Buildroot/build`), the strings are compared
against our own built GUI and the scripts against our built image; without one, against the
previous release's binary and this repository's overlay.

What goes in from a report, and how: the LGPL files of the helpers may be merged into
`src/`; the GPL-3 ones only into a separate program, as the release ships them; the buildroot
changes go to AetherX6100Buildroot (or this project's workflow patch steps); the features
named by the strings are written here, in our own code. Nothing of the closed binary is
taken but its words.
