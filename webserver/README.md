# OpenX6X00 web configurator

The radio's web pages (bands, digital modes, memories, cluster, Field Ops, logs,
gallery, backup, display, time, Ethernet, TCI server), served by the radio on
port 80 (and 443 with a self-signed certificate, for the Gallery's in-browser
conversion). Python 3 and [bottle](https://bottlepy.org/), LGPL-2.1
([LICENSE](LICENSE)).

Run on a PC against a copy of the radio's files:

```
python -m x6100_webserver --db params.db --port 8080 --https-port 0
```

`params.db` is the radio's `/mnt/params.db`. Pages that talk to the radio
screen (Display, Ethernet) need the GUI's configurator socket
(`/tmp/x6100_cfg.sock`, `src/cfg_socket.cpp`) and only work on the radio.

Options: `--host`, `--port`, `--https-port` (0 = off), `--cert-dir`, `--db`,
`--cluster-conf`, `--tci-conf`, `--channels-file`, `--filebrowser-path`,
`--funnel-dir`, `--debug`.

At its start the server writes the Panel and the Screen page for Tailscale
Funnel into `--funnel-dir` (`/run/x6100-funnel`; empty: not written): the
pages without the menu, their sockets on the page's own address, and the six
files they load. tailscaled serves that folder to the internet
(`rootfs/usr/sbin/x6100-tailscale`); this server answers no request from
there (one with `Tailscale-Funnel-Request` gets a 403).
