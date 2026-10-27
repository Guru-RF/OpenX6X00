# Pat (built from source, patched)

Pat is a Winlink email client by Martin Hebnes Pedersen (LA5NTA) and contributors, written in
Go. On the radio it is the Winlink client: its web mailbox on port 8080, ARDOP through
ardopcf, the frequency through the GUI's rigctld-compatible server.

Source: https://github.com/la5nta/pat
Tag: `v1.0.0`, commit `2e6a8d14baf0268f4e2aa4d01784a54ca935cf52` (2026-04-19). The tag is a
lightweight tag on an unsigned commit, so `build.sh` checks the commit and stops when it
differs.
License: MIT, Copyright (c) 2020 Martin Hebnes Pedersen (LA5NTA) (`LICENSE` in the source)

Not vendored: `build.sh` clones the tag at build time. `build.sh`, `patches/` and this file
are ours.

## The build (`build.sh`)

- Go: the official toolchain, `go1.27.3.linux-amd64.tar.gz` from go.dev, SHA-256
  `65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40` (from
  https://go.dev/dl/?mode=json&include=all). Pat v1.0.0 needs Go 1.24 or newer; buildroot's
  Go (1.20 in both trees) is too old, so Pat is built outside buildroot. The pin is in the
  script, not in the Dockerfile, so the container and the GitHub runner use the same Go.
- Downloaded once into `$HOME/.cache/openx6x00/go-<version>` (in the container
  `/work/.brhome/.cache`), with the hash checked before it is unpacked.
- The patches below applied in order with `git apply`, then `web/dist/js/*.map` removed (the
  web UI's source maps: 3 MB of the binary, used only by a browser's developer tools).
- `GOTOOLCHAIN=local GOFLAGS=-mod=readonly`, the public module proxy and checksum database,
  `go mod download` and `go mod verify`: a module that does not match `go.sum` stops the build.
- `CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -trimpath -ldflags="-s -w -buildid="`:
  one static binary for both radios, about 13.9 MB. Two builds with Go's build cache wiped in
  between gave the same SHA-256.
- The binary is kept as `$HOME/.cache/openx6x00/pat/pat-<tag>-go<version>-<hash>/pat`, the
  hash being that of the patches and of `build.sh`; a second run prints that path and
  downloads nothing. Go's module cache (`gopath/`) is read-only, as Go makes it:
  `chmod -R u+w` it before removing it.
- `buildroot/local-build.sh` passes the path to the GUI's CMake (`-DPAT_BINARY`), which
  installs it as `/usr/bin/pat`: it is in the card image and in the update bundle.

No `make.bash`: it runs the tests and its `embed` steps download data. The web UI's build
(`web/dist`), the fallback gateway list and the sunspot table are committed upstream, so no
node or npm either.

Without cgo, Pat loses the Linux kernel AX.25 engine and serial hamlib; ARDOP, VARA, PACTOR,
AGWPE, telnet and rigctld over TCP stay.

Name and version: Pat asks forks to consider another name and not to bump the version unless
they do (`internal/buildinfo/VERSION.go`), since the Winlink CMS uses it. We keep both: it
reports itself as Pat 1.0.0 with the upstream revision `2e6a8d1`; `go version -m` shows
`v1.0.0+dirty` (our patches).

## Local patches (`patches/`)

Each one made with `git format-patch` against `v1.0.0`; `git apply` takes them in order.

- `0001-http-guard.patch`: a guard around Pat's HTTP handler (`api/guard.go`, wrapped in
  `api.ListenAndServe`). Pat has no login, and a plain GET of `/api/connect` keys the
  transmitter, so a web page on another site could make the browser of anyone on the radio's
  network do it. The Host header must be an IP address, `localhost`, the hostname,
  `<hostname>.local`, end in `.ts.net`, or be the radio's MagicDNS short name (the first part
  of `name=` in `/run/tailscale/status`: `x6200-1` while the tailnet still has an earlier
  card's machine as `x6200`), the names the web configurator takes (no DNS rebinding). For
  `/api/` and `/ws`: an Origin header, when present, must name the same host:port as Host,
  and Sec-Fetch-Site, when present, must be `same-origin` or `none`. What changes something
  (`/api/connect` and `/api/disconnect` on any method, every API request that is not a GET or
  HEAD) must also come with an Origin or jQuery's `X-Requested-With`, which Pat's page sends:
  an `<img>` or a link on another site reaches a plain `http://` address on the LAN with
  neither Origin nor Sec-Fetch-Site. 403 otherwise. Pages are sent with
  `X-Frame-Options: SAMEORIGIN`, so another site cannot frame the mailbox and have its buttons
  clicked. With unit tests (`api/guard_test.go`).
- `0002-no-phone-home.patch`: the web UI no longer makes the radio contact the internet each
  time it is opened. `/api/new-release-check` (which asked api.getpat.io) answers 204, "no
  newer release": this Pat comes with the radio's own updates. `GET
  /api/winlink-account/password-recovery-email` (which sent the callsign and the password to
  winlink.org in the URL) answers a placeholder address, which the UI takes for "an address
  is set". Setting an address (PUT) still goes to winlink.org.

Not a patch: the version report (daily) and the app's own recovery-email check (every 14 days),
both to api.winlink.org, stay off through `version_reporting_disabled: true` in the config,
which the supervisor writes.

What still goes to the internet, and only when there is one: the Winlink account check at a
connect (cached 100 days), the gateway list download (the list built in is the fallback), the
forms update, a HEAD request to www.google.com when a form is opened, and from Pat's
configuration page the account check when the callsign is typed, the account sign-up and the
recovery-email address when one is set.

## Licences of what is compiled in

Pat itself, wl2k-go, Pat-Vara and ptc-go: MIT. The other Go modules (`go version -m` of the
binary):

- MIT: adrg/xdg, aymerick/douceur, clipperhouse/stringish, clipperhouse/uax29, creack/goselect,
  kelseyhightower/envconfig, mattn/go-runewidth, pd0mz/go-maidenhead, peterh/liner,
  go.uber.org/multierr
- BSD: dario.cat/mergo, albenik/go-serial, fsnotify/fsnotify, gorilla/css, gorilla/mux,
  gorilla/websocket (2-clause), howeyc/crc16, microcosm-cc/bluemonday, paulrosania/go-charset,
  spf13/pflag, golang.org/x/net, x/sync, x/sys, x/term
- ISC: nfnt/resize
- Apache-2.0: bndr/gotabulate
- MPL-2.0: hashicorp/go-version v1.8.0, unmodified; its source is at
  https://github.com/hashicorp/go-version
- GPL-3.0 or Apache-2.0, at the user's choice: gorhill/cronexpr; used here under Apache-2.0

The web UI bundle compiled in (`web/dist`): MIT (Bootstrap 3.2, jQuery, URI.js,
bootstrap-select, bootstrap-tokenfield; `web/dist/static/LICENSES.md` in the source, served by
Pat at `/dist/static/LICENSES.md`).
