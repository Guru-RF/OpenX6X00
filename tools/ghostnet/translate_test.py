#!/usr/bin/env python3
# Host test of the translation helper (rootfs/usr/sbin/x6100-translate) with a stand-in for MyMemory: nothing
# goes to the network. What a request holds (no callsign, locator, e-mail or "de" parameter, the User-Agent),
# the head of a message kept here, the placeholders put back, the language pairs, the cache, the daily limit and
# the errors. Run with python3 -I from the Makefile; the cache goes in data/.
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import sys
import urllib.error
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-translate")

loader = importlib.machinery.SourceFileLoader("x6100_translate", HELPER)
spec = importlib.util.spec_from_loader("x6100_translate", loader)
tr = importlib.util.module_from_spec(spec)
loader.exec_module(tr)

os.makedirs(os.path.join(HERE, "data"), exist_ok=True)

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAILED:", what, file=sys.stderr)


class Answer:
    def __init__(self, body):
        self.body = body.encode()

    def read(self):
        return self.body

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class MyMemory:
    """Stands in for urllib.request.urlopen: records each request, answers as told"""

    def __init__(self):
        self.requests = []
        self.answer = None      # a function of the query's text, or an exception to raise

    def __call__(self, req, timeout=None):
        self.requests.append(req)
        if isinstance(self.answer, Exception):
            raise self.answer
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(req.full_url).query)
        return Answer(self.answer(q["q"][0], q["langpair"][0]))


def ok(text):
    return json.dumps({"responseData": {"translatedText": text}, "responseStatus": 200})


fake = MyMemory()
tr.urllib.request.urlopen = fake


def run(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        try:
            tr.main(["x6100-translate"] + list(args))
        except SystemExit:
            pass
    return out.getvalue().strip()


runs = 0


def fresh():
    """A cache of its own for the next case (make test empties data/ before the run)"""
    global runs
    runs += 1
    tr.CACHE = os.path.join(HERE, "data", f"translate-{os.getpid()}-{runs}.json")   # new also when run again
    fake.requests.clear()


# The head stays here
head, body = tr.split_head("CX1ABC: @GHOSTNET CHECKING IN FROM MONTEVIDEO ALL WELL")
check(head == "CX1ABC: @GHOSTNET" and body == "CHECKING IN FROM MONTEVIDEO ALL WELL", f"split_head {head!r} {body!r}")
head, body = tr.split_head("ON6URE: CX1ABC MSG PLEASE CALL ME")
check(head == "ON6URE: CX1ABC MSG" and body == "PLEASE CALL ME", f"split_head MSG {head!r} {body!r}")

# Callsigns, locators, groups, macros and e-mail addresses as placeholders
masked, kept = tr.mask("PLEASE RELAY TO ON6URE/P AT JO20 OR MAIL me@example.org, <CALL> @GSTFLASH 5 PM")
check("ON6URE" not in masked and "JO20" not in masked and "example" not in masked and "<CALL>" not in masked
      and "@GSTFLASH" not in masked, f"mask {masked!r}")
check(kept == ["ON6URE/P", "JO20", "me@example.org", "<CALL>", "@GSTFLASH"], f"kept {kept!r}")
check("5 PM" in masked and "[3]," in masked, f"plain numbers and punctuation stay {masked!r}")
check(tr.unmask("Por favor retransmita a [1] en [ 2 ] o correo [3], [4] [5] 5 PM", kept)
      == "Por favor retransmita a ON6URE/P en JO20 o correo me@example.org, <CALL> @GSTFLASH 5 PM", "unmask")
check(tr.unmask("Por favor retransmita", ["CX1ABC"]) == "Por favor retransmita CX1ABC", "a lost placeholder at the end")
check(tr.unmask("Mensaje [9]", ["CX1ABC"]) == "Mensaje [9] CX1ABC", "an unknown placeholder left as it is")

# The language pairs
EN_TEXT, ES_TEXT = "ALL STATIONS PLEASE CHECK IN TONIGHT", "BUENAS NOCHES A TODOS LAS ESTACIONES"
check(tr.pair_for("rx", "en", EN_TEXT) is None, "rx: English for an English GUI")
check(tr.pair_for("rx", "es", EN_TEXT) == "en|es", "rx: English into Spanish")
check(tr.pair_for("rx", "nl", EN_TEXT) == "en|nl", "rx: English into Dutch")
check(tr.pair_for("rx", "nl", ES_TEXT) == "es|nl", "rx: Spanish into Dutch")
check(tr.pair_for("rx", "en", ES_TEXT) == "es|en", "rx: Spanish into English")
check(tr.pair_for("rx", "es", ES_TEXT) is None, "rx: Spanish for a Spanish GUI")
check(tr.pair_for("rx", "nl", "QRV SOON 73") is None, "rx: no telling the language, nothing asked")
check(tr.pair_for("ask", "nl", "QRV SOON") == "en|nl", "ask: from English when it can't tell")
check(tr.pair_for("ask", "en", EN_TEXT) is None, "ask: English for an English GUI")
check(tr.pair_for("ask", "zh-CN", ES_TEXT) == "es|zh-CN", "ask: Spanish into Chinese")
check(tr.pair_for("tx", "en", ES_TEXT) is None, "tx: an English GUI")
check(tr.pair_for("tx", "es", ES_TEXT) == "es|en", "tx: Spanish into English")
check(tr.pair_for("tx", "es", EN_TEXT) is None, "tx: English already")

# A request: what goes out, what comes back
fresh()
fake.answer = lambda q, pair: ok("Comprobando desde [1], todo bien en [2]")
got = run("rx", "es", "CX1ABC: @GHOSTNET CHECKING IN FROM ON6URE ALL WELL HERE AT JO20")
check(got == "ok Comprobando desde ON6URE, todo bien en JO20", f"rx answer {got!r}")
check(len(fake.requests) == 1, "one request")
req = fake.requests[0]
url = req.full_url
query = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
check(url.startswith("https://api.mymemory.translated.net/get?"), f"MyMemory's address {url}")
check(set(query) == {"q", "langpair"}, f"only q and langpair, no de= e-mail {sorted(query)}")
check(query["langpair"] == ["en|es"], f"langpair {query['langpair']}")
check(query["q"] == ["Checking in from [1] all well here at [2]"], f"q {query['q']}")
for secret in ("ON6URE", "CX1ABC", "JO20", "GHOSTNET", "on6ure"):
    check(secret not in url, f"{secret} not in the request")
agent = req.get_header("User-agent") or ""
check(agent.startswith("OpenX6X00/") and "github.com/Guru-RF/OpenX6X00" in agent, f"User-Agent {agent!r}")

# The cache: the same text again, no request
got = run("rx", "es", "CX1ABC: @GHOSTNET CHECKING IN FROM ON6URE ALL WELL HERE AT JO20")
check(got == "ok Comprobando desde ON6URE, todo bien en JO20" and len(fake.requests) == 1, f"cached {got!r}")
# another station sending the same words: the same masked text, still no request
got = run("rx", "es", "CX2XYZ: @GHOSTNET CHECKING IN FROM LU1AAA ALL WELL HERE AT GF15")
check(got == "ok Comprobando desde LU1AAA, todo bien en GF15" and len(fake.requests) == 1, f"cached, masked {got!r}")
cache = json.load(open(tr.CACHE))
check(all("ON6URE" not in k for k in cache["done"]), "the cache keys hold no callsign")
# the cache keeps 300
for i in range(310):
    cache["done"][f"en|es|text {i}"] = "x"
tr.save_cache(cache)
check(len(json.load(open(tr.CACHE))["done"]) == 300, "300 texts kept")

# The operator's text into English, in what JS8 sends, the head kept as typed
fresh()
fake.answer = lambda q, pair: ok("Good evening everyone, how are you? [1] from Montevideo")
got = run("tx", "es", "@GHOSTNET BUENAS NOCHES A TODOS, ¿CÓMO ESTÁN? CX8ANM DESDE MONTEVIDEO")
check(got == "ok @GHOSTNET GOOD EVENING EVERYONE, HOW ARE YOU? CX8ANM FROM MONTEVIDEO", f"tx {got!r}")
query = urllib.parse.parse_qs(urllib.parse.urlsplit(fake.requests[0].full_url).query)
check(query["langpair"] == ["es|en"] and "CX8ANM" not in fake.requests[0].full_url, f"tx request {query}")
check(tr.js8_text("¡Atención! Ñandú “quoted” 中文") == "ATENCION! NANDU \"QUOTED\"", "js8_text")

# Nothing asked: an English GUI's tx, only callsigns and numbers, a bad language code
fresh()
check(run("tx", "en", "@GHOSTNET HELLO ALL") == "skip", "tx with an English GUI")
check(run("ask", "es", "CX1ABC: ON6URE JO20 599") == "skip", "only callsigns")
check(run("ask", "es;id", "HELLO ALL STATIONS") == "error usage", "a bad language code")
check(run("rx", "es", "@HB HEARTBEAT") == "skip", "a heartbeat, all head")
check(not fake.requests, "none of them asked MyMemory")

# The daily limit: MyMemory's warning as the translation
fresh()
fake.answer = lambda q, pair: json.dumps({
    "responseData": {"translatedText": "MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY. "
                                       "NEXT AVAILABLE IN  10 HOURS 20 MINUTES 05 SECONDS VISIT ..."},
    "responseStatus": 429, "responseDetails": "MYMEMORY WARNING: ..."})
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "limit 11", "limit from the answer")
check(run("ask", "es", "SOMETHING ELSE TO TRANSLATE NOW") == "limit 11" and len(fake.requests) == 1,
      "nothing asked while the limit lasts")
# ... or as an HTTP error
fresh()
fake.answer = urllib.error.HTTPError(tr.API, 429, "Too Many Requests", {},
                                     io.BytesIO(b'{"responseDetails": "NEXT AVAILABLE IN 3 HOURS 00 MINUTES"}'))
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "limit 3", "limit from an HTTP 429")

# Errors
fresh()
fake.answer = urllib.error.URLError("no route")
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "error net", "no connection")
fake.answer = TimeoutError()
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "error net", "a timeout")
fake.answer = lambda q, pair: "<html>busy</html>"
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "error answer", "not JSON")
fake.answer = lambda q, pair: json.dumps({"responseData": {"translatedText": ""}, "responseStatus": 200})
check(run("ask", "es", "ALL STATIONS PLEASE CHECK IN") == "error answer", "no translation")
fake.answer = lambda q, pair: json.dumps({"responseData": {"translatedText": "x"}, "responseStatus": 403,
                                          "responseDetails": "'ES|XX' IS AN INVALID TARGET LANGUAGE"})
check(run("ask", "xx", "ALL STATIONS PLEASE CHECK IN") == "error answer", "a language MyMemory refuses")
check(not os.path.exists(tr.CACHE) or not json.load(open(tr.CACHE))["done"], "failures are not cached")

# The answer's HTML entities and spaces
fresh()
fake.answer = lambda q, pair: ok("Estaci&oacute;n  &quot;lista&quot;\n")
check(run("ask", "es", "STATION READY FOR THE NET") == 'ok Estación "lista"', "entities")

print(f"translate_test: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
