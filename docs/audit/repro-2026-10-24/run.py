#!/usr/bin/env python3
"""Reproduce the defects recorded at 00f91e1; every mutation stays in temporary state."""

import os
from pathlib import Path
import re
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
        print("#85 TUNE for an external tuner, the Panel's side:\n" + command(["node", HERE / "tune-page.js"], env,
              contains=("All tune page cases as they should be",)).strip())

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
        print("#85 TUNE for an external tuner from the Panel, a lease:\n" + command([binary, "tune"], env, contains=(
            "the stalled page's tune stopped by the radio: 1, the page told: 1, as the page gone: 1",
            "late renewals of the ended tune started one: 0",
            "a new press started one: 1, still on after its renewals: 1",
            "tune 0 stopped it: 1, as asked, not as the page gone: 1",
            "a viewer: err view only; no TX: err this session may not transmit; control at the radio: 1; "
            "the radio on the air: 1, queued=0; tunes started=0",
            "the page told: 1",
            "a tune started after the page let go: 0",
            "the radio's tune kept by the leases: 1, by a viewer's tune 0: 1, stopped by the page's tune 0: 1")).strip())
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
                         "indep, VFO key: band=3 A=21000000 B=7030000 vfo=1",
                         "indep, restart: band=3 vfo=1 A=21000000 B=7030000",
                         "indep, band key into split: band=9 A=21000000 B=21010000 split=1")),
            ("settings-rollback.cpp", ("after rollback and retry volume=42 memory=42",)),
            # Issue #104: a transverter's TX power limit and PWR under it
            ("xvtr-power.cpp", ("old settings: limit 1=0 2=0, at 145.5 MHz 0 W", "12 W asked: 10 W",
                                "limits: 1=0.5 2=0.1",
                                "cap: 2 m=0.5 20 m=0 20 m split to 2 m=0.5 2 m split to 20 m=0.5 "
                                "2 m split to 70 cm=0.1 above the range=0",
                                "transverter 1 off: 2 m=0",
                                "onto 2 m: held pwr=0.5 held=10 top=0.5",
                                "10 W set on 2 m (the knob, the Panel, TCI, CAT): pwr=0.5",
                                "turned down to 0.2 W, off 2 m: restored pwr=10 held=0 top=10",
                                "0.3 W onto 2 m: same pwr=0.3 held=0 top=0.5",
                                "0.3 W off 2 m: same pwr=0.3 held=0 top=10",
                                "onto 2 m at 0.5 W: held pwr=0.5 held=10 top=0.5",
                                "back on 70 cm: held pwr=1 held=10 top=1",
                                "turned down to 0.3 W, onto 2 m: same pwr=0.3 held=10 top=0.5",
                                "off both: restored pwr=10 held=0 top=10",
                                "Settings, Off to 0.1 W: held pwr=0.1 held=0.8 top=0.1", "0.2 W: held pwr=0.2 held=0.8",
                                "1.0 W: held pwr=0.8 held=0.8 top=1", "down to 0.4 W: held pwr=0.4 held=0.8",
                                "Off, on 2 m still: same pwr=0.4 held=0.8 top=10",
                                "then off 2 m: restored pwr=0.8 held=0 top=10",
                                "restart: limit 1=0.5 2=1 pwr=0.5 held=10 row pwr_max=5",
                                "restart, off 2 m: restored pwr=10 held=0 top=10",
                                "on battery, off 2 m: restored pwr=5 held=0 top=5")),
        ):
            binary = work / Path(source).stem
            command([*CXX, "-std=c++20", *common, HERE / source, *cfg, *gc,
                     "-pthread", "-lsqlite3", "-o", binary], env)
            print(source + ":\n" + command([binary], env, contains=markers).strip())

        # Issue #91: the Panel's Filter pop-up against the radio's filter rules: the production SettingsManager
        # with the remote server's own P_FILTER case, as src/remote_server.c has it
        server = (REPO / "src/remote_server.c").read_text()
        start = server.index("        case P_FILTER:")
        (work / "panel_filter.inc").write_text(server[start:server.index("        case P_ATU:", start)])
        binary = work / "filter-rules"
        command([*CXX, "-std=c++20", *common, "-I" + directory, HERE / "filter-rules.cpp", *cfg,
                 "src/cfg/computed_api.cpp", *gc, "-pthread", "-lsqlite3", "-o", binary], env)
        print("#91 the Panel's filter presets and edges, as the radio keeps them:\n" + command(
            ["node", HERE / "panel-filter.js", binary], env, contains=("filter cases as they should be",)).strip())

        binary = work / "adif-frequency"
        command([*CC, *cflags, *san, HERE / "adif-frequency.c", *gc, "-o", binary], env)
        command([binary], env)
        print("B03 (fixed): the ADIF formatter writes 145.5 MHz within its buffer")

        binary = work / "ws-handshake"
        command([*CC, *cflags, *san, HERE / "ws-handshake.c", *gc, "-o", binary], env)
        print("C08 WebSocket names (26 October audit, fixed), Tailscale Funnel's requests (#58), the radio's own "
              "names (#105), Pat's mailbox (port 8080) refused on the radio (#105 review) and the https page's "
              "sockets on its own port (#58):\n" + command(
            [binary], env, contains=("63 of 63 cases as they should be",)).strip())
        binary = work / "ws-handshake-pc"
        command([*CC, *cflags, *san, "-DHOST_BUILD", HERE / "ws-handshake.c", *gc, "-o", binary], env)
        print("The same on the PC build, whose web configurator is on port 8080:\n" + command(
            [binary], env, contains=("58 of 58 cases as they should be",)).strip())

        # The X6100's GUI knows its mic's codes only; the X6200's the H-22's besides (#56: its table off a radio)
        for board, flags, known, h22 in (
                ("X6100", [], "known 25, unknown 230", (
                    "X6100, H-22 3, 0x18: nothing; log: hand mic: key 0x18 (unknown)",
                    "X6100, H-22 #, 0x28: typed F-INP; log: hand mic: key 0x28 (F-INP)",
                    "X6100, H-22 VOL+, 0x14: nothing; log: hand mic: key 0x14 (unknown)",
                    "X6100, the H-22: 25 of 25 keys as they should be",
                    "X6100, 0x18 held: nothing; VOL+ down while held: 0, after: 0")),
                ("X6200", ["-DBOARD_X6200"], "known 40, unknown 215", (
                    "X6200, H-22 3, 0x18: press 3, release 3; log: hand mic: key 0x18 (3)",
                    "X6200, H-22 *, 0x1F: press ., release .; log: hand mic: key 0x1F (.)",
                    "X6200, H-22 HOME, 0x24: typed CE; log: hand mic: key 0x24 (CE)",
                    "X6200, H-22 F3, 0x13: press F3, release F3; log: hand mic: key 0x13 (F3)",
                    "X6200, H-22 A/B, 0x16: press A/B, release A/B; log: hand mic: key 0x16 (A/B)",
                    "X6200, the H-22: 25 of 25 keys as they should be",
                    "X6200, 0x18 held: press 3, long 3, long release 3; VOL+ down while held: 0, after: 0",
                    "X6200, 0x14 held: press VOL+, long VOL+, long release VOL+; VOL+ down while held: 1, after: 0"))):
            binary = work / ("mic-keys-" + board.lower())
            command([*CC, *cflags, *flags, *san, HERE / "mic-keys.c", "src/mic_keys.c", *gc, "-o", binary], env)
            print(f"#56 hand mic keys, the {board}'s GUI: every code into the log, each key of both mics:\n" + command(
                [binary], env, contains=(
                    "main screen, 0xE4: press 3, release 3; log: hand mic: key 0xE4 (3)",
                    "main screen, 0x53: nothing; log: hand mic: key 0x53 (unknown)",
                    "a list focused, 0xE2: typed 2; log: hand mic: key 0xE2 (2)",
                    "a text window open, 0xE1: typed 1 into the text window; log: nothing",
                    "a text window open, 0x53: nothing; log: nothing",
                    "codes 0x01-0xFF: logged 255, " + known,
                    board + ", the X6100 mic: 25 of 25 keys as they should be", *h22)).strip())

        # What the main screen does with the H-22's own keys (#56): ab_key(), the VOL+/VOL- repeat and mic_key_cb()
        # cut out of screen_main.c, each from where it starts to the end of the function named
        screen = (REPO / "src/screen_main.c").read_text()

        def cut(start, function):
            a = screen.index(start)
            b = screen.index("{", screen.index(function, a)) + 1
            level = 1
            while level:
                level += (screen[b] == "{") - (screen[b] == "}")
                b += 1
            return screen[a:b] + "\n"

        (work / "mic_screen_keys.c").write_text(cut("static void ab_key(", "static void ab_key(") +
                                                cut("#define MIC_VOL_DELAY_MS", "static void mic_key_cb("))
        binary = work / "mic-screen"
        command([*CC, *cflags, *san, "-I" + str(work), HERE / "mic-screen.c", *gc, "-o", binary], env)
        print("#56 the H-22's own keys on the main screen:\n" + command([binary], env, contains=(
            "F3 let go: action 3", "F3 held: action 13",
            "VOL+ held 0.4 s: volume 32, \"Volume: 32\", every 100 ms",
            "VOL+ let go after the long press: timer deleted",
            "VOL+ let go, the release to another screen (walkie mode): timer deleted",
            "A/B held: VFO copied, \"Clone VFO B->A\"", "A/B let go, an app holding the VFOs: nothing",
            "BAND UP let go: band up", "42 of 42 cases as they should be")).strip())

        # Issue #111: the user's frequency lock and an app's hold, cut out of screen_main.c: from the end of
        # cw_mode() to the end of toggle_freq_lock(), and main_screen_lock_freq() to main_screen_freq_locked()
        def end_of(function):
            b = screen.index("{", screen.index(function)) + 1
            level = 1
            while level:
                level += (screen[b] == "{") - (screen[b] == "}")
                b += 1
            return b

        (work / "freq_lock_cut.c").write_text(
            "#include <stdbool.h>\n#include <stdint.h>\n"
            "typedef struct SubjectInt SubjectInt;\n"
            "int32_t subject_i_get(SubjectInt *s);\nvoid subject_i_set(SubjectInt *s, int32_t v);\n"
            "void freq_strip_set_lock(bool user_lock);\nvoid info_lock_mode(bool lock);\n"
            "static SubjectInt *freq_lock;\nstatic bool user_lock, app_lock;\n"
            "static bool band_lock, mode_lock, ab_lock;\n" +
            screen[end_of("static bool cw_mode("):end_of("toggle_freq_lock(void) {")] + "\n" +
            screen[screen.index("void main_screen_lock_freq("):end_of("bool main_screen_freq_locked(")] + "\n"
            "void lock_key(void) { toggle_freq_lock(); }\n"
            "void lock_init(SubjectInt *s) { freq_lock = s; }\n")
        binary = work / "freq-lock"
        command([*CC, *cflags, *san, HERE / "freq-lock.c", work / "freq_lock_cut.c", "-o", binary], env)
        print("#111 the user's frequency lock and an app's hold:\n" + command([binary], env, contains=(
            "FT8 closed, the lock set before it: locked 1, user's lock 1, LOCK shown 1",
            "FT8 opened, no lock: locked 1, user's lock 0, LOCK shown 0",
            "the Panel's LOCK off, FT8 open: locked 1, user's lock 0, LOCK shown 0",
            "JS8 closed: locked 1, user's lock 1, LOCK shown 1",
            "16 of 16 cases as they should be")).strip())

        # Issue #49: the battery % shown, the base's or the voltage's; down at the steep end of the curve the
        # voltage's stays, where it met a base stuck at 0 % and gave 0 % with half an hour still in the pack,
        # and is taken there for a base at 0 % from the start (the radio switched on at 7.3 V)
        binary = work / "bat-cap"
        command([*CC, *cflags, *san, HERE / "bat-cap.c", "src/bat_watch.c", *gc, "-o", binary], env)
        print("#49 the battery % shown:\n" + command([binary], env, contains=(
            "base at 0-3 %: 8.2 V 98, 8.1 V 94, 8.0 V 88, 7.9 V 84, 7.8 V 80, 7.7 V 73, 7.6 V 65, 7.5 V 56, 7.4 V 42, "
            "7.3 V 24, 7.2 V 9, 7.3 V 24, 7.2 V 9, 7.1 V 3, 7.0 V 1, 6.9 V 1, 6.8 V 0, 6.5 V 0\n",
            "base too high: 7.8 V 91, 7.7 V 90, 7.6 V 88, 7.5 V 56, 7.4 V 42, 7.3 V 24, 7.2 V 9, 7.1 V 3, 7.0 V 1, "
            "6.8 V 0, 6.6 V 0, 6.5 V 0\n",
            "base that counts right: 8.3 V 100, 8.2 V 97, 8.0 V 90, 7.8 V 78, 7.6 V 63, 7.4 V 45, 7.3 V 26, 7.2 V 12, "
            "7.1 V 4, 6.9 V 1, 6.5 V 0\n",
            "base at 15 %: 7.5 V 56, 7.4 V 42, 7.3 V 24, 7.2 V 9, 7.1 V 3, 6.9 V 1\n",
            "base at 0 % from 7.3 V: 7.3 V 24, 7.2 V 9, 7.3 V 24, 7.2 V 9, 7.1 V 3, 7.0 V 1, 6.8 V 0\n",
            "base that counts right from 7.3 V: 7.3 V 26, 7.2 V 12, 7.2 V 5, 7.1 V 3, 6.9 V 1, 6.8 V 0, 6.5 V 0\n",
            "base back after a charge: 8.1 V 94, 8.1 V 95, 8.0 V 90\n",
            "base back at 7.4 V: 7.5 V 56, 7.4 V 42, 7.4 V 35, 7.3 V 22\n")).strip())

        # Issue #108: on external power, battery_log.csv rows replayed through bat_est.c one by one; with the charger
        # off the plug from the first row (it was the lightning sign and a time to full for a minute), a flag that
        # comes one row late keeps its time to full from the second, at 90 % and more too, and two rows late there
        # from the third (the end of a charge does not count the row a stretch starts with)
        binary = work / "bat-est"
        command([*CC, *cflags, *san, HERE / "bat-est.c", "src/bat_est.c", *gc, "-o", binary], env)
        print("#108 the time to full on the supply:\n" + command([binary], env, contains=(
            "Charger Off at 60 %: 60 % plug, 60 % plug, 60 % plug, 60 % plug, 60 % plug\n",
            "Charger Off at 92 %: 92 % plug, 92 % plug, 92 % plug, 92 % plug, 92 % plug\n",
            "charging from 60 %: 60 % bolt ~6h31, 60 % bolt ~6h31, 61 % bolt ~6h16, 61 % bolt ~6h16, 62 % bolt ~6h01\n",
            "charging from 86 %, the flag one row late: 86 % plug, 87 % bolt ~1h44, 87 % bolt ~1h44, 87 % bolt ~1h44, "
            "88 % bolt ~1h37\n",
            "charging from 92 %, the flag one row late: 92 % plug, 92 % bolt ~1h06, 93 % bolt ~0h57, 93 % bolt ~0h57, "
            "94 % bolt ~0h49\n",
            "charging from 92 %, the flag two rows late: 92 % plug, 92 % plug, 92 % bolt ~1h06, 93 % bolt ~0h57, "
            "93 % bolt ~0h57\n",
            "charging at 70 %, the flag off for a row: 70 % bolt ~4h03, 70 % bolt ~4h03, 71 % bolt ~3h54, "
            "71 % bolt ~3h54, 71 % bolt ~3h54\n",
            "charged to 97 %: 96 % bolt ~0h33, 97 % bolt ~0h25, 97 % plug, 97 % plug, 97 % plug, 97 % plug\n")).strip())
        # Issue #101: bootanim/bootanim.c on the PC for each radio, its framebuffer a file and the radio's libraries
        # stand-ins (bootanim-host.c): the owner's boot picture, and ours when that one can't be read
        include = work / "bootanim-include"
        for name in ("gpiod.h", "aether_radio/x6100_control/control.h", "aether_radio/x6200_control/control.h"):
            (include / name).parent.mkdir(parents=True, exist_ok=True)
            (include / name).write_text('#include "bootanim-stubs.h"\n')
        wraps = ",".join("--wrap=" + f for f in ("open", "ioctl", "access", "usleep", "clock_gettime",
                                                 "png_image_begin_read_from_file", "sqlite3_open_v2"))
        animations = []
        for board, flags in (("x6100", []), ("x6200", ["-DBOARD_X6200"])):
            animations.append(work / ("bootanim-" + board))
            command([*CC, *cflags, *flags, "-U_FORTIFY_SOURCE", "-I" + str(include), "-I" + str(HERE),
                     "bootanim/bootanim.c", HERE / "bootanim-host.c", "-Wl," + wraps, "-lpng", "-lsqlite3", "-lasound",
                     "-lm", "-pthread", "-o", animations[-1]], env)
        print("#101 the boot picture:\n" + command([sys.executable, HERE / "boot_picture_checks.py", *animations], env,
              contains=("The boot picture (#101): 76 of 76 cases as they should be",)).strip())

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

        # Issue #92: the mode of a QSO logged by hand, the app open naming it; no FT8 in USB-D without one
        binary = work / "qso-mode"
        command([*CC, *cflags, *san, HERE / "qso-mode.c", "src/field/flog.c", *gc, "-o", binary], env)
        print("#92 the mode of a QSO logged by hand:\n" + command([binary], env, contains=(
            "PSK31, USB-D, 14070.000 kHz: PSK31", "PSK31, USB-D, 14074.500 kHz: PSK31",
            "Olivia, USB-D, 14071.000 kHz: OLIVIA", "JTTY, USB-D, 14090.000 kHz: JTTY",
            "RTTY, LSB-D, 14080.000 kHz: RTTY", "RTTY, CW, 14080.000 kHz: CW", "no app, USB-D, 14074.000 kHz: FT8",
            "no app, USB-D, 14070.000 kHz: (no mode)", "no app, USB, 14200.000 kHz: SSB",
            "record of PSK31: MODE PSK SUBMODE PSK31", "record of OLIVIA: MODE OLIVIA SUBMODE -",
            "record of JTTY: MODE MFSK SUBMODE JTTY", "record of RTTY: MODE RTTY SUBMODE -",
            "record of a QSO with no mode: MODE - SUBMODE -", "the log's header: <ADIF_VER:5>3.1.8 ",
            "read back: PSK31 PSK63 OLIVIA JTTY RTTY FT4 -", "36 of 36 cases as they should be")).strip())
        # The apps by the names the frequency strip gives them (screen_main.c's app_name, which
        # main_screen_digital_app returns): a name changed there would log the radio's mode instead
        strip = (REPO / "src/screen_main.c").read_text()
        strip = strip[strip.index("static const char *app_name("):]
        names = set(re.findall(r'"([^"]+)"', strip[:strip.index("\n}\n")]))
        names |= set(re.findall(r'"(FT[0-9])"', (REPO / "src/ft8/ft2.h").read_text()))   # ftx_protocol_name
        print("#92 the apps' names on the frequency strip, logged in USB-D:\n" + command(
            [binary, "app", *sorted(names)], env, contains=(
                "FT8 -> FT8", "FT4 -> FT4", "FT2 -> FT2", "JS8 -> JS8", "PSK31 -> PSK31", "PSK63 -> PSK63",
                "Olivia -> OLIVIA", "JTTY -> JTTY", "RTTY -> RTTY", "SSTV -> SSTV", "WSPR -> WSPR")).strip())

    print("All reproduced findings and the selected original-fix checks matched the audits.")


if __name__ == "__main__":
    main()
