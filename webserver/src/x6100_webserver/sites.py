"""The rules against requests sent by other sites, shared by the configurator's pages (apps.refuse_other_sites) and
SmartCW's page on a port of its own (smartcw.py), which reads its requests itself, without bottle."""
import urllib.parse

# tailscaled sets it on every request it hands on from the internet through Tailscale Funnel, and drops one a browser
# sent (#58)
FUNNEL_HEADER = "Tailscale-Funnel-Request"


def host_name(host):
    """The name or address of a Host header (host: its value, None without one), lower case, without port and
    trailing dot; "" for none, or one that is no name ("[::1", which no browser sends)"""
    try:
        return (urllib.parse.urlsplit("//" + (host or "")).hostname or "").rstrip(".")
    except ValueError:
        return ""


def origin_ok(origin, host):
    """An Origin header (origin) of the page's own site: the name in the Host header (host) and its port. "null" (a
    file:// page) has no name and is refused too. A browser names the port in Origin, as in Host, only when it is not
    the scheme's own (80, 443): the radio's pages, by http, https, the tailnet or on a PC on a port of their own, send
    the same port in both"""
    try:
        theirs = urllib.parse.urlsplit(origin)
        ours = urllib.parse.urlsplit("//" + (host or ""))
        same_port = theirs.port is None or theirs.port == ours.port
    except ValueError:   # no port number, or no name ("[::1")
        return False
    return bool(theirs.hostname) and theirs.hostname.lower() == (ours.hostname or "").lower() and same_port
