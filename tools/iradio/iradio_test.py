#!/usr/bin/env python3
"""x6100-iradio on a PC, without the network: the station lists from saved answers of radio-browser.info (every
format with ffmpeg on the image, MP3 only without it), the countries, the playlists, what a station's answer is by its
type and which decoder takes it, the ICY titles taken out of the audio, the server names from the DNS, the cache, and
the refusal at play time of what an image without ffmpeg cannot play.

  python3 -I tools/iradio/iradio_test.py [-v]    (X6100_IRADIO=path: another copy of the script)

The answers in fixtures/ are in the shape radio-browser.info's /json/stations/search and /json/countries give
(api.radio-browser.info), with made-up stations on example.org: the corners are a codec other than MP3, HLS by its
flag and by its address, a repeated address, a tab and a line break in a name, an address over 250 bytes, a name over
60 bytes of UTF-8, no name, an ftp address, odd bitrates, country codes and uuids.
"""
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "fixtures")
SCRIPT = os.environ.get("X6100_IRADIO") or os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-iradio")


def load():
    loader = importlib.machinery.SourceFileLoader("x6100_iradio", SCRIPT)
    spec = importlib.util.spec_from_loader("x6100_iradio", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


ir = load()
MP3_ONLY = staticmethod(lambda: False)
EVERY = staticmethod(lambda: True)


class Formats:
    """ffmpeg there (EVERY) or not (MP3_ONLY) for the test, whatever this PC has"""
    formats = MP3_ONLY

    def setUp(self):
        self._saved_formats = ir.all_formats
        ir.all_formats = type(self).formats

    def tearDown(self):
        ir.all_formats = self._saved_formats


def fixture(name, mode="r"):
    with open(os.path.join(FIX, name), mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        return json.load(f) if name.endswith(".json") else f.read()


class StationList(unittest.TestCase):
    def test_saved_answer(self):
        rows = ir.station_rows(fixture("stations_be.json"))
        self.assertEqual(rows[0], "Radio Example One\thttp://stream.example.org/one.mp3\tMP3\t128\tBE\t"
                                  "96062a7b-0601-11e8-ae97-52543be04c81")
        # "mp3" is MP3; the resolved address before the one given; the tab and the line break out of the name
        self.assertEqual(rows[1], "Example Jazz Lounge\thttps://edge.example.net/jazz-128.mp3\tMP3\t192\tBE\t"
                                  "a1b2c3d4-0000-4000-8000-000000000002")
        # no resolved address: the one given; bitrate 0: none
        self.assertEqual(rows[2], "Only the url field\thttp://only.example.org:8000/stream\tMP3\t\tBE\t"
                                  "a1b2c3d4-0000-4000-8000-000000000008")
        # a name over 60 bytes is cut on a whole character, and stays valid UTF-8
        name = rows[3].split("\t")[0]
        self.assertTrue(name.startswith("Ràdio Ëxample ünïcode — "))
        self.assertLessEqual(len(name.encode()), 60)
        self.assertGreater(len(name.encode()), 56)
        # bitrate 99999, country "BEL": neither kept; an upper-case uuid kept in lower case
        self.assertEqual(rows[4], "Upper uuid, odd country\thttp://cc.example.org/s\tMP3\t\t\t"
                                  "a1b2c3d4-0000-4000-8000-00000000000d")
        self.assertEqual(len(rows), 5, "AAC, Ogg, UNKNOWN, HLS (flag and .m3u8), a repeat, a long address, no name "
                                       "and ftp are left out:\n" + "\n".join(rows))
        for row in rows:
            f = row.split("\t")
            self.assertEqual(len(f), 6)
            self.assertEqual(f[2], "MP3")
            self.assertLessEqual(len(f[1].encode()), 250)

    def test_every_format(self):
        """With ffmpeg: every codec, HLS too; still no repeat, long address, nameless or ftp station"""
        rows = ir.station_rows(fixture("stations_be.json"), every=True)
        got = [(r.split("\t")[0][:20], r.split("\t")[2]) for r in rows]
        self.assertEqual(got, [("Radio Example One", "MP3"), ("Example Jazz Lounge", "MP3"), ("Example AAC", "AAC"),
                               ("Example HLS flag", "MP3"), ("Example HLS ending", "MP3"), ("Example Ogg", "OGG"),
                               ("Only the url field", "MP3"), ("Ràdio Ëxample ünïcod", "MP3"),
                               ("Upper uuid, odd coun", "MP3"), ("Unknown codec", "")])
        hls = dict(fixture("stations_be.json")[3], codec="UNKNOWN")
        self.assertEqual(ir.station_row(hls, every=True).split("\t")[2], "HLS", "an HLS station of no known codec")
        self.assertIsNone(ir.station_row(hls), "no HLS without ffmpeg")

    def test_limit_and_junk(self):
        self.assertEqual(len(ir.station_rows(fixture("stations_be.json"), limit=2)), 2)
        self.assertEqual(ir.station_rows(None), [])
        self.assertEqual(ir.station_rows({"name": "x"}), [])
        self.assertEqual(ir.station_rows(["x", 3, None]), [])

    def test_codec_filter(self):
        base = fixture("stations_be.json")[0]
        for codec, mp3_only, every in (("MP3", "MP3", "MP3"), ("mp3", "MP3", "MP3"), (" MP3 ", "MP3", "MP3"),
                                       ("AAC", None, "AAC"), ("AAC+", None, "AAC+"), ("OGG", None, "OGG"),
                                       ("FLAC", None, "FLAC"), ("UNKNOWN", None, ""), ("", None, ""),
                                       (None, None, ""), ("A VERY LONG CODEC", None, "")):
            s = dict(base, codec=codec)
            row = ir.station_row(s)
            self.assertEqual(row and row.split("\t")[2], mp3_only, codec)
            self.assertEqual(ir.station_row(s, every=True).split("\t")[2], every, codec)
        self.assertNotIn("codec", ir.COMMON)
        self.assertEqual(ir.COMMON["hidebroken"], "true")


class Countries(unittest.TestCase):
    def test_saved_answer(self):
        rows = ir.country_rows(fixture("countries.json"))
        self.assertEqual(rows, ["Åland Islands\tAX\t3", "Belgium\tBE\t412", "Netherlands\tNL\t1203", "Number Sign\tNS\t1",
                                "The United Kingdom Of Great Britain And Northern Ireland\tGB\t2301",
                                "Uruguay\tUY\t188"])

    def test_junk(self):
        self.assertEqual(ir.country_rows({"a": 1}), [])
        self.assertEqual(ir.country_rows([None, "BE", {"iso_3166_1": "BE"}]), [])


class Playlists(unittest.TestCase):
    def test_pls(self):
        self.assertEqual(ir.playlist_urls(fixture("playlist.pls")),
                         (["http://pls.example.org:8000/live", "http://pls-backup.example.org:8000/live"], False))

    def test_m3u(self):
        text = fixture("playlist.m3u", "rb").decode("utf-8")   # a byte order mark, CR LF, a repeat, a comment
        self.assertEqual(ir.playlist_urls(text), (["https://m3u.example.org/stream.mp3", "http://m3u2.example.org/s"],
                                                  False))

    def test_hls(self):
        self.assertEqual(ir.playlist_urls(fixture("playlist_hls.m3u8")), ([], True))

    def test_asx_and_xspf(self):
        self.assertEqual(ir.playlist_urls(fixture("playlist.asx")), (["http://asx.example.org/stream?id=1&q=hi"], False))
        self.assertEqual(ir.playlist_urls(fixture("playlist.xspf")), (["http://xspf.example.org/live.mp3"], False))

    def test_nothing(self):
        self.assertEqual(ir.playlist_urls(""), ([], False))
        self.assertEqual(ir.playlist_urls("<html><body>Listen live!</body></html>"), ([], False))


class StreamKind(unittest.TestCase):
    def test_types(self):
        cases = [
            ("audio/mpeg", "", ("mp3", "MP3")), ("audio/mpeg; charset=x", "", ("mp3", "MP3")),
            ("Audio/MP3", "", ("mp3", "MP3")), ("audio/aacp", "", ("other", "AAC")),
            ("audio/aac", "http://x/s.mp3", ("other", "AAC")), ("application/ogg", "", ("other", "Ogg")),
            ("audio/flac", "", ("other", "FLAC")), ("audio/x-scpls", "", ("playlist", "")),
            ("audio/x-mpegurl", "", ("playlist", "")), ("application/vnd.apple.mpegurl", "", ("playlist", "")),
            ("video/x-ms-asf", "", ("playlist", "")), ("text/plain", "", ("playlist", "")),
            ("text/html; charset=utf-8", "", ("page", "text/html")), ("application/json", "", ("page", "application/json")),
            ("audio/x-weird", "", ("other", "X-WEIRD")),
            # no type, or a type that says nothing: by the address's ending
            ("", "http://x/live.mp3?id=2", ("mp3", "MP3")), ("application/octet-stream", "http://x/a.aac", ("other", "AAC")),
            (None, "http://x/list.pls", ("playlist", "")), ("", "http://x/a.opus", ("other", "Opus")),
            ("", "http://x:8000/stream", ("unknown", "")), ("application/octet-stream", "", ("unknown", "")),
        ]
        for ctype, url, want in cases:
            self.assertEqual(ir.stream_kind(ctype, url), want, (ctype, url))


class Icy(unittest.TestCase):
    def test_titles(self):
        self.assertEqual(ir.icy_title(b"StreamTitle='Artist - Song';StreamUrl='';\0\0\0\0"), "Artist - Song")
        self.assertEqual(ir.icy_title(b"StreamTitle='Guns N' Roses - Track';\0"), "Guns N' Roses - Track")
        self.assertEqual(ir.icy_title("StreamTitle='Caf\xe9 \t live';".encode("latin-1")), "Caf\xe9 live")
        self.assertEqual(ir.icy_title("StreamTitle='Ünïcode';".encode()), "Ünïcode")
        self.assertEqual(ir.icy_title(b"StreamTitle='';"), "")
        self.assertIsNone(ir.icy_title(b"StreamUrl='http://x';\0"))

    def test_pump(self):
        meta = b"StreamTitle='First';".ljust(32, b"\0")
        src = io.BytesIO(b"A" * 16 + bytes([2]) + meta + b"B" * 16 + b"\0" + b"C" * 5)
        dst, titles = io.BytesIO(), []
        ir.pump(src, dst, 16, titles.append)
        self.assertEqual(dst.getvalue(), b"A" * 16 + b"B" * 16 + b"C" * 5)
        self.assertEqual(titles, ["First"])
        dst = io.BytesIO()
        ir.pump(io.BytesIO(b"x" * 20000), dst, 0, titles.append)     # no metadata: all of it is audio
        self.assertEqual(len(dst.getvalue()), 20000)


class Reasons(unittest.TestCase):
    def test_reason(self):
        import socket
        import urllib.error
        self.assertEqual(ir.reason(urllib.error.URLError(OSError(101, "Network is unreachable"))), "Network is unreachable")
        self.assertEqual(ir.reason(urllib.error.URLError(socket.timeout("timed out"))), "timed out")
        self.assertEqual(ir.reason(ValueError("x " * 60)), ("x " * 35).strip())


class Servers(unittest.TestCase):
    def test_names(self):
        back = {"1.2.3.4": "de1.api.radio-browser.info.", "5.6.7.8": "evil.example.com",
                "1.1.1.1": "DE1.api.radio-browser.info", "2.2.2.2": "nl1.api.radio-browser.info",
                "3.3.3.3": "x.api.radio-browser.info.example.com"}

        def lookup(a):
            if a not in back:
                raise OSError("no PTR")
            return back[a]
        self.assertEqual(ir.server_names(["1.2.3.4", "5.6.7.8", "9.9.9.9", "1.1.1.1", "2.2.2.2", "3.3.3.3"], lookup),
                         ["de1.api.radio-browser.info", "nl1.api.radio-browser.info"])


class Cache(Formats, unittest.TestCase):
    def setUp(self):
        Formats.setUp(self)
        self.dir = tempfile.mkdtemp(prefix="iradio-")
        self.saved = ir.CACHE, ir.api
        ir.CACHE = os.path.join(self.dir, "cache")
        self.calls = []

    def tearDown(self):
        ir.CACHE, ir.api = self.saved
        shutil.rmtree(self.dir)
        Formats.tearDown(self)

    def fake(self, answers):
        def api(path, params=None, tries=3, timeout=10):
            self.calls.append((path, dict(params or {})))
            a = answers.pop(0)
            if isinstance(a, Exception):
                raise a
            return a
        ir.api = api

    def test_country_cached(self):
        self.fake([fixture("stations_be.json")])
        rows = ir.cmd_country(["be"])
        self.assertEqual(len(rows), 5)
        self.assertEqual(self.calls, [("/json/stations/search", dict(ir.COMMON, countrycode="BE", codec="MP3"))])
        self.assertEqual(ir.cmd_country(["BE"]), rows, "the second time from the cache")
        self.assertEqual(len(self.calls), 1)
        self.assertTrue(os.path.exists(os.path.join(ir.CACHE, "country-BE-mp3.tsv")))

    def test_country_every_format(self):
        """With ffmpeg the directory is asked for every codec, and that list is kept apart from the MP3 one"""
        ir.all_formats = EVERY
        self.fake([fixture("stations_be.json")])
        self.assertEqual(len(ir.cmd_country(["BE"])), 10)
        self.assertEqual(self.calls, [("/json/stations/search", dict(ir.COMMON, countrycode="BE"))])
        self.assertTrue(os.path.exists(os.path.join(ir.CACHE, "country-BE.tsv")))
        ir.all_formats = MP3_ONLY
        self.fake([fixture("stations_be.json")])
        self.assertEqual(len(ir.cmd_country(["BE"])), 5, "not the list made with ffmpeg")

    def test_search_by_tag_when_few(self):
        few = fixture("stations_be.json")[:2]
        self.fake([few, fixture("stations_be.json")])
        rows = ir.cmd_search(["Jazz", "Lounge"])
        self.assertEqual([c[1].get("name") or c[1].get("tag") for c in self.calls], ["Jazz Lounge", "jazz lounge"])
        self.assertEqual(len(rows), 5)
        self.assertTrue(os.path.exists(os.path.join(ir.CACHE, "search-jazz_lounge-mp3.tsv")))

    def test_search_enough_by_name(self):
        many = [dict(s, url_resolved="http://m.example.org/%d" % i, stationuuid="") for i, s in
                enumerate(fixture("stations_be.json")[:1] * 20)]
        self.fake([many])
        self.assertEqual(len(ir.cmd_search(["one"])), 20)
        self.assertEqual(len(self.calls), 1, "15 or more by name: the tags are not asked")

    def test_none_found_not_cached(self):
        self.fake([[], [], fixture("stations_be.json"), []])
        with self.assertRaises(ir.Fail):
            ir.cmd_search(["nothing"])
        self.assertEqual(len(ir.cmd_search(["nothing"])), 5)

    def test_countries(self):
        self.fake([fixture("countries.json")])
        self.assertEqual(len(ir.cmd_countries([])), 6)
        self.assertEqual(self.calls, [("/json/countries", {"hidebroken": "true"})])
        self.assertEqual(len(ir.cmd_countries([])), 6)
        self.assertEqual(len(self.calls), 1)

    def test_main_lines(self):
        self.fake([ir.Fail("no answer from radio-browser.info (timed out)"), fixture("stations_be.json")])
        out, keep = io.StringIO(), sys.stdout
        sys.stdout = out
        try:
            self.assertEqual(ir.main(["x6100-iradio", "top"]), 1)
            self.assertEqual(ir.main(["x6100-iradio", "top"]), 0)
            self.assertEqual(ir.main(["x6100-iradio", "country", "BEL"]), 1)
        finally:
            sys.stdout = keep
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0], "error no answer from radio-browser.info (timed out)")
        self.assertEqual(len(lines), 1 + 5 + 1)
        self.assertEqual(lines[-1], "error no country chosen")


class FakeResponse:
    def __init__(self, ctype, body=b"", url=""):
        self.headers = {"Content-Type": ctype}
        self.body, self.url, self.closed = io.BytesIO(body), url, False

    def read(self, n=-1):
        return self.body.read(n)

    def geturl(self):
        return self.url

    def close(self):
        self.closed = True


class OpenStream(Formats, unittest.TestCase):
    """What play does with the station's first answers, before any audio, on an image without ffmpeg: no network, no
    decoder"""

    def setUp(self):
        Formats.setUp(self)
        self.saved = ir.http_open
        self.asked = []

    def tearDown(self):
        ir.http_open = self.saved
        Formats.tearDown(self)

    def answers(self, table):
        def http_open(url, headers=None, timeout=15):
            self.asked.append(url)
            ctype, body = table[url]
            return FakeResponse(ctype, body, url)
        ir.http_open = http_open

    def test_playlist_to_mp3(self):
        self.answers({"http://x/list.pls": ("audio/x-scpls", fixture("playlist.pls").encode()),
                      "http://pls.example.org:8000/live": ("audio/mpeg", b"\xff\xfb")})
        r, kind, name = ir.open_stream("http://x/list.pls")
        self.assertEqual((r.url, kind, name), ("http://pls.example.org:8000/live", "mp3", "MP3"))
        self.assertEqual(self.asked, ["http://x/list.pls", "http://pls.example.org:8000/live"])

    def test_refusals(self):
        self.answers({"http://x/aac": ("audio/aacp", b""), "http://x/hls": ("application/vnd.apple.mpegurl",
                      fixture("playlist_hls.m3u8").encode()), "http://x/page": ("text/html", b"<html>"),
                      "http://x/empty.m3u": ("audio/x-mpegurl", b"#EXTM3U\n")})
        for url, words in (("http://x/aac", "AAC stream: " + ir.NOT_HERE),
                           ("http://x/hls", "HLS stream: " + ir.NOT_HERE),
                           ("http://x/page", "not a stream: a web page (text/html)"),
                           ("http://x/empty.m3u", "not a stream: no audio address in it")):
            with self.assertRaises(ir.Fail) as c:
                ir.open_stream(url)
            self.assertEqual(str(c.exception), words)
            self.assertTrue(c.exception.final, "not tried again: " + url)

    def test_unknown_is_tried(self):
        self.answers({"http://x:8000/": ("", b"\xff\xfb")})
        r, kind, name = ir.open_stream("http://x:8000/")
        self.assertEqual((r.url, kind), ("http://x:8000/", "unknown"))
        self.assertEqual(ir.decoder(kind, name, False), (ir.MPG123, "MP3"), "without ffmpeg mpg123 tries it")

    def test_loop(self):
        self.answers({"http://x/a.m3u": ("audio/x-mpegurl", b"http://x/a.m3u\n")})
        with self.assertRaises(ir.Fail) as c:
            ir.open_stream("http://x/a.m3u")
        self.assertTrue(c.exception.final)


class Decoders(Formats, unittest.TestCase):
    """With ffmpeg on the image: MP3 still with mpg123, the rest and HLS with ffmpeg"""
    formats = EVERY

    def setUp(self):
        Formats.setUp(self)
        self.saved = ir.http_open

    def tearDown(self):
        ir.http_open = self.saved
        Formats.tearDown(self)

    def answers(self, table):
        ir.http_open = lambda url, headers=None, timeout=15: FakeResponse(table[url][0], table[url][1], url)

    def test_choice(self):
        ff = [ir.FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "quiet", "-i", "pipe:0"] + ir.PCM_OUT
        self.assertEqual(ir.decoder("mp3", "MP3", False), (ir.MPG123, "MP3"))
        self.assertEqual(ir.decoder("mp3", "MP3", True), (ff, "MP3"), "an MP3 that mpg123 could not decode")
        self.assertEqual(ir.decoder("other", "AAC", False), (ff, "AAC"))
        self.assertEqual(ir.decoder("unknown", "", False), (ff, "Audio"))
        self.assertIn("s16le", ir.PCM_OUT)
        self.assertEqual(ir.PCM_OUT[-5:], ["-ac", "1", "-ar", "44100", "pipe:1"])

    def test_streams(self):
        self.answers({"http://x/aac": ("audio/aacp", b""), "http://x/live.m3u8": ("application/vnd.apple.mpegurl",
                      fixture("playlist_hls.m3u8").encode()), "http://x/page": ("text/html", b"<html>")})
        r, kind, name = ir.open_stream("http://x/aac")
        self.assertEqual((kind, name), ("other", "AAC"))
        self.assertEqual(ir.open_stream("http://x/live.m3u8"), (None, "hls", "http://x/live.m3u8"))
        with self.assertRaises(ir.Fail):
            ir.open_stream("http://x/page")     # a web page stays refused


if __name__ == "__main__":
    unittest.main()
