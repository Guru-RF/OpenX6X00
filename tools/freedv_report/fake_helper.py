#!/usr/bin/env python3
"""A stand-in for x6100-freedv-reporter in freedv_report_test.c. FAKE_MODE says how it behaves, FAKE_LOG where it
writes what it got (its arguments, each line, "eof" at the end of stdin):
  echo      says "connected", logs every line; at the end of stdin it logs "eof" and exits
  deaf      reads nothing, ignores the end of stdin, sleeps (SIGTERM ends it)
  stubborn  as deaf, and ignores SIGTERM too (SIGKILL ends it)
  quit      exits at once
"""
import os
import signal
import sys
import time

mode, log = os.environ.get("FAKE_MODE", "echo"), os.environ.get("FAKE_LOG", "/dev/null")


def note(text):
    with open(log, "a") as f:
        f.write(text + "\n")


note("args " + "|".join(sys.argv[1:]))
if mode == "quit":
    sys.exit(1)
if mode in ("deaf", "stubborn"):
    if mode == "stubborn":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    note("asleep")
    while True:
        time.sleep(1)
os.write(1, b"connected\n")
os.write(1, b"qsy K1ABC 7177000 pse QSY\n")
buf = b""
while True:
    data = os.read(0, 4096)
    if not data:
        note("eof")
        sys.exit(0)
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        note("line " + line.decode("utf-8", "replace"))
