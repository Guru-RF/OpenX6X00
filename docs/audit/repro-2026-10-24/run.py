#!/usr/bin/env python3
"""Reproduce the defects recorded at 00f91e1; every mutation stays in temporary state."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
CC = shlex.split(os.environ.get("CC", "cc"))
CXX = shlex.split(os.environ.get("CXX", "c++"))


def command(args, env, *, failure=False, contains=()):
    result = subprocess.run([str(a) for a in args], cwd=REPO, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ok = result.returncode != 0 if failure else result.returncode == 0
    if not ok or any(marker not in result.stdout for marker in contains):
        print(result.stdout, end="")
        raise SystemExit("Unexpected result: " + shlex.join(str(a) for a in args))
    return result.stdout


def main():
    env = os.environ.copy()
    # LeakSanitizer needs tracing permissions; this audit uses access checks only.
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    with tempfile.TemporaryDirectory(prefix="openx6x00-audit-") as directory:
        work = Path(directory)
        env["AUDIT_TMP"] = directory
        common = ["-g", "-O1", "-DLV_DRAW_NEON=0", "-ffunction-sections",
                  "-fdata-sections", "-I.", "-Isrc", "-Ilvgl", "-Ibuild-host/deps/include"]
        cflags = ["-std=c11", "-D_GNU_SOURCE", *common]
        san = ["-fsanitize=address", "-fno-omit-frame-pointer"]
        gc = ["-Wl,--gc-sections"]

        for script in ("web_checks.py", "log_checks.py", "updater_checks.py", "backup_checks.py", "tailscale_checks.py"):
            output = command([sys.executable, HERE / script], env)
            print(script + ":\n" + output.strip())

        print("B10 PTT renewal:\n" + command(["node", HERE / "ptt-renewal.js"], env).strip())
        print("C09 keyboard scope:\n" + command(["node", HERE / "keyboard-scope.js"], env,
              contains=("keyboard cases as they should be",)).strip())
        print("C12 edit flow:\n" + command(["node", HERE / "edit-flow.js"], env,
              contains=("edit-flow cases as they should be",)).strip())
        print("#58 Tailscale Funnel, the pages:\n" + command(["node", HERE / "funnel-pages.js"], env,
              contains=("funnel page cases as they should be",)).strip())

        # 26 October audit: the browser remote's commands, on the page and in the radio
        print("C02 PTT hold, the page:\n" + command(["node", HERE / "ptt-hold.js"], env).strip())
        command([sys.executable, HERE / "make-remote-harness.py"], env)
        remote = work / "remote"
        binary = remote / "remote"
        command([*CC, *cflags, "-I" + str(remote), "-I" + str(HERE), remote / "remote-commands.c",
                 remote / "control_owner.c", "src/tci_ws.c", "-pthread", "-o", binary], env)
        print("C02 ping numbers, the radio (fixed):\n" + command([binary, "ping"], env, contains=(
            "pongs: pong|pong 7|pong 9007199254740991|",)).strip())
        print("C02 PTT hold, the radio (fixed):\n" + command([binary, "ptt"], env, contains=(
            "late renewals of the ended hold keyed the radio: 0",
            "a new press keyed the radio: 1, still keyed at its renewal: 1",
            "the refused press's renewal keyed the radio: 0",
            "a page without numbers keyed the radio: 0, told to reload: 1")).strip())
        print("C03 log receipts, two tabs of one browser (fixed):\n" + command([binary, "log"], env, contains=(
            "one number, two contacts: writes=1, the second told ok=0, told conflict=1",
            "two tabs, two more contacts: writes=3, 53-bit numbers answered whole=1")).strip())
        print("C13 refused settings (fixed):\n" + command([binary, "set"], env, contains=(
            "view only: queued=0 err=1 state again=1", "control at the radio: queued=0 err=1 state again=1",
            "frequency locked: queued=0 err=1 state again=1", "out of range: queued=0 err=1 state again=1",
            "a mode this radio has not: queued=0 err=1 state again=1", "GUI queue full: queued=0 err=1 state again=1",
            "the queue-full refusal: err vol: the radio is busy, try again",
            "no such setting: queued=0 err=1 state again=1", "accepted: queued=1 err=0 state again=0")).strip())
        print("C02/C03/C13 (26 October audit): the remote's checks matched the fixes")
        print("#58 the pairing code's waits:\n" + command([binary, "pair"], env, contains=(
            "a code: shown=2, the same code asked again 10 s later=1",
            "asked at once after a burned code: err pairing paused: a new code in 60 s",
            "burned codes, then the wait for a new one in s: 60 120 240 480 960 1920 3600 3600",
            "Pairing paused 60 min: three wrong codes",
            "asked at the radio: a code at once=1, burned again: paused 1 min, not an hour=1",
            "the right code: paired=1; a guess with no code shown counted=0")).strip())
        print("#58 SHA-256 (the standard's examples, the padding's edges):\n" + command([binary, "sha"], env, contains=(
            "sha256(0 bytes) = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "sha256(3 bytes) = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256(56 bytes) = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            "sha256(55 bytes) = 72fa96f64bf2dd082aafb08cf80ed17f5b3d2c6cd527c4b138b69506f5e5c173",
            "sha256(56 bytes) = 67e4026bfbe6f1cd3f40518f324bcdf4426ae00faf5a0cddeae67a0e60ecf665",
            "sha256(64 bytes) = a8ae6e6ee929abea3afcfc5258c8ccd6f85273e0d4626d26c7279f3250f77c8e",
            "sha256(1000000 bytes) = cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "same=1, differs in the last byte=0")).strip())
        print("#58 a browser over Tailscale Funnel:\n" + command([binary, "funnel"], env, contains=(
            "over Funnel: pair? and pair refused=1, a code shown=0, tries counted=0",
            "over Funnel, a wrong key: closed=1, counted=1; on the LAN: closed=0, watching=1")).strip())
        print("#58 Use away from home:\n" + command([binary, "away"], env, contains=(
            'away: answered with a key=1, its session "Kitchen phone (away)" operate tx=1',
            "away again: a new key=1, the one before gone=1, sessions 3 -> 3",
            "the admin's away key: admin",
            "from the internet: err an away link is made at home",
            "unpaired: err pair this browser first",
            "with its away key at home: err this browser has its away link already",
            'two browsers named alike for 24 characters: "Club station field lapto (away)", both away keys good=1',
            "the first one asks again: its own replaced=1, the other's kept=1",
            "a file from before the home column: 2 sessions, its away key replaced=1, the new one knows its browser=1",
            "revoked at home and paired again: its away key from before replaced=1, sessions 2")).strip())
        print("#58 session tokens kept hashed:\n" + command([binary, "tokens"], env, contains=(
            "a file from before: 2 sessions loaded, the tokens in the file now=0",
            "3eb1bd439947eb762998e566ccc2e099c791118b2f40579cc4f7da2b5061b7f9\tShack PC\t2\t1",
            "4ba68aa8767bde72e8c798ee82d1275291cea73e72ad74d35ecf48e41386eb82\tKitchen phone\t1\t1",
            "the phone paired before the update: operates",
            "its token with the last digit changed: err unknown session",
            "read again: 2 sessions, the PC paired before: admin",
            "a new pairing: token handed out=1, in the file=0, its hash in the file=1")).strip())

        command([sys.executable, HERE / "make-unkey-harness.py"], env)
        binary = work / "unkey"
        command([*CC, *cflags, "-I" + directory, work / "unkey-ownership.c",
                 work / "control_owner.c", "-pthread", "-o", binary], env)
        print("B01 TCI unkey (fixed):\n" + command([binary], env,
              contains=("after local input and explicit unkey: keyed=0, tx_session_end_calls=1",
                        "after listen-only and explicit unkey: keyed=0, tx_session_end_calls=2")).strip())

        command([sys.executable, HERE / "make-log-harness.py"], env)
        binary = work / "log-identities"
        command([*CC, *cflags, work / "log-identities.c", "-pthread", "-o", binary], env)
        print("B07 log receipts (fixed):\n" + command([binary], env,
              contains=("browser 0: logged 1 ok", "browser 1: logged 1 unknown", "writes=2")).strip())

        cfg = ["src/cfg/settings_manager.cpp", "src/cfg/subject.cpp",
               "src/cfg/pending_writes.cpp", "src/cfg/storage_policy.cpp", "src/cfg/db.cpp"]
        for source, markers in (
            ("vfo.cpp", ("after B=7100000: band=7 A=14100000 B=7100000 mode=2", "persisted B=7100000",
                         "after toggle: band=4 A=14100000 B=7100000 vfo=1",
                         "indep, band key: band=9 A=21000000 B=7030000",
                         "indep, VFO key: band=3 A=21000000 B=7030000 vfo=1")),
            ("settings-rollback.cpp", ("after rollback and retry volume=42 memory=42",)),
        ):
            binary = work / Path(source).stem
            command([*CXX, "-std=c++20", *common, HERE / source, *cfg, *gc,
                     "-pthread", "-lsqlite3", "-o", binary], env)
            print(source + ":\n" + command([binary], env, contains=markers).strip())

        binary = work / "adif-frequency"
        command([*CC, *cflags, *san, HERE / "adif-frequency.c", *gc, "-o", binary], env)
        command([binary], env)
        print("B03 (fixed): the ADIF formatter writes 145.5 MHz within its buffer")

        binary = work / "ws-handshake"
        command([*CC, *cflags, *san, HERE / "ws-handshake.c", *gc, "-o", binary], env)
        print("C08 WebSocket names (26 October audit, fixed), and Tailscale Funnel's requests (#58):\n" + command(
            [binary], env, contains=("29 of 29 cases as they should be",)).strip())

        binary = work / "mic-keys"
        command([*CC, *cflags, *san, HERE / "mic-keys.c", "src/mic_keys.c", *gc, "-o", binary], env)
        print("#56 hand mic keys, every code into the log:\n" + command([binary], env, contains=(
            "main screen, 0xE4: press 3, release 3; log: hand mic: key 0xE4 (3)",
            "main screen, 0x53: nothing; log: hand mic: key 0x53 (unknown)",
            "a list focused, 0xE2: typed 2; log: hand mic: key 0xE2 (2)",
            "a text window open, 0xE1: typed 1 into the text window; log: nothing",
            "a text window open, 0x53: nothing; log: nothing",
            "codes 0x01-0xFF: logged 255, known 25, unknown 230")).strip())

        fixture = work / "truncated.adi"
        fixture.write_bytes(b"<CALL:5")
        binary = work / "adif-input"
        command([*CC, *cflags, *san, HERE / "adif-input.c", HERE / "guard-malloc.c",
                 "src/adif.c", *gc, "-Wl,--wrap=malloc", "-Wl,--wrap=free", "-o", binary], env)
        command([binary, fixture], env, contains=("records=0",))
        print("B04 (fixed): a file ending in a length's digits is read within its bytes")

        fixture.write_bytes(b"<eor>" + b" " * 1009 + b"<CALL:15>")
        binary = work / "field-input"
        command([*CC, *cflags, *san, HERE / "field-input.c", "src/field/flog.c", *gc,
                 "-o", binary], env)
        command([binary, fixture], env, contains=("records=0",))
        print("B06 (fixed): the Field Ops reader skips a record whose field runs past it")

    print("All reproduced findings and the selected original-fix checks matched the audits.")


if __name__ == "__main__":
    main()
