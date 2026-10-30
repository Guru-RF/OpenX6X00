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
  network do it. Nothing that came through Tailscale Funnel, from the internet: tailscaled's serve
  proxy marks such a request with a `Tailscale-Funnel-Request` header, and Pat stays on the
  radio's network and the tailnet whatever is funneled by mistake. The Host header must be an IP address, `localhost`, the hostname,
  `<hostname>.local`, `<hostname>.<domain>` for a domain the radio's network gives it (the
  `search` and `domain` lines of `/etc/resolv.conf`, which NetworkManager writes; not a domain
  that is a public suffix alone, such as `nl` or `co.uk`), a name listed in
  `/mnt/own_names.conf` (the web configurator's Network page, #105), end in `.ts.net`, or be the
  radio's MagicDNS short name (the first part of `name=` in `/run/tailscale/status`: `x6200-1`
  while the tailnet still has an earlier card's machine as `x6200`), the names the web
  configurator takes (no DNS rebinding). Pat reads the two files itself at each request, by the
  same rule as the web configurator and the radio's sockets (`src/tci_ws.c`). For
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
- `0003-ardop-bandwidth-cap.patch`: the config's ARDOP bandwidth (`ardop.arq_bandwidth`, which
  the radio's Winlink page sets) is the widest a connection may use. Pat's gateway list fills in
  each gateway's widest bandwidth (`bw=2000MAX` for one listed as ARDOP 2000), the connect dialog
  takes it over, the browser remembers it, and a connection's `bw=` wins over the config: at the
  radio's 7 W at most, a 200 Hz session connected where 2000MAX did not (#18). `Connect()` lowers an ARDOP
  `bw=` above the config's Max to `<Max>MAX` and logs it ("ARDOP bandwidth 500MAX, the widest set
  on the radio (Operating > Winlink), not the 2000MAX asked"); a gateway that runs a wider
  bandwidth takes it and negotiates down. A `bw=` at or under it, or none, is left as it is.
  With a test (`app/ardop_bandwidth_test.go`).
- `0004-log-append.patch`: Pat's log (`--log`, the mailbox's log pane) is opened with `O_APPEND`
  (still emptied at each start), where Pat used `os.Create`. The supervisor adds lines of its own
  to it ("OpenX6X00: ...": a refused transmission, what to do after a failed connection, no
  password set), which Pat's next line wrote over before, and trims it, which left Pat's next line
  behind a hole of zeros. The log pane's `tailFile` reads a file that became shorter than what it
  read again from its start. With a test (`api/tail_test.go`).
- `0005-web-ui-xss.patch`: what a received message, another station or the network sends is shown
  as text in the web mailbox, never as markup. A Winlink message reaches the inbox from anyone who
  writes to `<call>@winlink.org`, and Pat's page put strings from it into the page with jQuery's
  `.html()` (or `.append()` of a string): an attachment named `<img src=x onerror=...>` (the name
  comes unchanged from the message's `File:` line) ran as script on Pat's own page, port 8080,
  which can read and send mail, connect and transmit, and open the radio's TCI socket. Now text
  (`.text()`, text nodes, attributes set with `.attr()`): the message window (date, From, To, Cc,
  the attachments' names, their addresses now encoded), the message list (addresses, date,
  message ID; `htmlEscape` escapes quotes too), the log pane, the status text (the remote's
  address), the prompt of the messages to download (senders, subjects), the status bar's
  messages, the composer's and the password recovery's errors, the connect dialog's aliases, the
  gateway list's prediction and the forms' catalog. The message body was sanitized already
  (bluemonday). The template page shows `/api/template`'s preview as markup: the insertion tags'
  values (the subject, sender and body of the message replied to among them) go into it escaped.
  With a test (`internal/forms/preview_test.go`). An attachment is sent as it came in the message
  (`serveAttachment`), never as a page of Pat's: before, an `.html` or `.svg` one opened as a page
  (its type guessed from its name or its first bytes) in a sandbox that let its scripts run, send
  requests to Pat and open pop-ups. Now `Content-Disposition: attachment`,
  `application/octet-stream`, `X-Content-Type-Options: nosniff` and `Content-Security-Policy:
  default-src 'none'; style-src 'unsafe-inline'; sandbox` (runs and loads nothing), and no
  `Access-Control-Allow-Origin: null`. A JPEG, PNG or GIF (the pictures the message window
  shows) is still shown in a tab, inline as its own type, under the same policy; the message
  window saves the others under their names (`download`) rather than opening a tab. A form Pat
  renders (`rendertohtml=true`) keeps upstream's sandbox, which forms need, and gets nosniff. With
  a test (`api/attachment_test.go`). The patch changes `web/src` and, to match it,
  the minified `web/dist/js/app.js` by hand (no node in the build: `--word-diff` of the patched
  tree against `v1.0.0` shows those changes); `config.js` and `template.js` need none. It should
  go upstream, its `web/src` part (upstream rebuilds `web/dist`); a report for la5nta/pat is
  drafted, not sent.

`pat_ui_test.js` (ours, not in a patch) runs Pat's page and `app.js` from a patched source in
jsdom, against a stand-in for Pat's server that answers with such names and lines: `NODE_PATH=<a
node_modules with jsdom> node tools/pat/pat_ui_test.js <patched source>` (its first lines say how
to make one). Without 0005, 7 of its 40 checks pass.

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
