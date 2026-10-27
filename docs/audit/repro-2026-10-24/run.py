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

        for script in ("web_checks.py", "log_checks.py", "updater_checks.py", "backup_checks.py"):
            output = command([sys.executable, HERE / script], env)
            print(script + ":\n" + output.strip())

        print("B10 PTT renewal:\n" + command(["node", HERE / "ptt-renewal.js"], env).strip())

        # 26 October audit: the browser remote's commands, on the page and in the radio
        print("C02 PTT hold, the page:\n" + command(["node", HERE / "ptt-hold.js"], env).strip())
        command([sys.executable, HERE / "make-remote-harness.py"], env)
        remote = work / "remote"
        binary = remote / "remote"
        command([*CC, *cflags, "-I" + str(remote), "-I" + str(HERE), remote / "remote-commands.c",
                 remote / "control_owner.c", "-pthread", "-o", binary], env)
        print("C02 ping numbers, the radio (fixed):\n" + command([binary, "ping"], env, contains=(
            "pongs: pong|pong 7|pong 9007199254740991|",)).strip())
        print("C02 PTT hold, the radio (fixed):\n" + command([binary, "ptt"], env, contains=(
            "late renewals of the ended hold keyed the radio: 0",
            "a new press keyed the radio: 1, still keyed at its renewal: 1",
            "the refused press's renewal keyed the radio: 0",
            "a page without numbers keyed the radio: 0, told to reload: 1")).strip())

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
                         "after toggle: band=4 A=7040000 B=7100000 vfo=1")),
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

    print("All 12 reproduced findings and the selected original-fix checks matched the audit.")


if __name__ == "__main__":
    main()
