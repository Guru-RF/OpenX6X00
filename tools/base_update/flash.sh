#!/bin/bash
#
# Writes Xiegu's base firmware 1.0.8 (or 1.0.7 again) into the X6200's base from OpenX6X00, without Xiegu's system:
# base_flash (tools/base_update/base_flash.c) does what Xiegu's MENU > SYSTEM > UPGRADE does, step for step. The base
# file comes from tools/base_update/fetch.sh, the program from tools/base_update/build.sh; both are copied to the
# radio. More in tools/base_update/README.md. The X6100's base too, with --file (Xiegu's V1.1.8 or V1.1.9, or a test
# build with --test-ok); base_flash tells the radio from its device tree.
#
#     tools/base_update/flash.sh [root@x6200.local] --check     reads only: the file, the base's version, who holds
#                                                                  its port and bus. Nothing is sent to the base.
#     tools/base_update/flash.sh [root@x6200.local] --flash     writes the base (asks first; --yes does not ask)
#     tools/base_update/flash.sh [root@x6200.local] --recover   after a write that failed past the erase
#     ... --to VERSION                                             another release than 1.0.8 (1.0.7, 1.0.6, 1.0.3):
#                                                                  its file in build/base_update, found by its SHA-256
#                                                                  in base_releases.txt whatever its name (fetch.sh
#                                                                  VERSION puts it there); the newest build of a version
#                                                                  that has two (1.0.7: the radio's own, then xiegu.eu's)
#     ... --file FILE.xgf [--unknown-ok]                           a file of your own; one the list does not know only
#                                                                  with --unknown-ok, and typed confirmation
#     ... --test-ok                                                a test build base_flash knows (RF.Guru's
#                                                                  X6100-Baseband), with typed confirmation
#     tools/base_update/flash.sh root@x6100.local --dry-run --file F   the X6100's only: into the base's bootloader and
#                                                                  out again, nothing erased or written (the base is
#                                                                  reset twice; the GUI is stopped meanwhile)
#
# As Xiegu asks for its own update: the radio on a 13.8 V supply of 3 A or more, the battery out, for the whole
# write (about half a minute; the X6100 about a minute). The GUI is stopped for it and started again after; meanwhile /var/run/S95gui.lock is
# held, so a GUI start waits for the end of the write. The write runs on the radio by itself (run_on_radio.sh under
# setsid, its log in /tmp/base_update/flash.log, then appended to /mnt/base_update.log): a lost connection does not
# stop it, and this script follows the log until it ends. Do not switch the radio off while it runs. After it the
# X6200's base does not start the new firmware by itself: the radio switches itself off a minute or two later, and is
# switched on again by hand; then --check shows the new version. The X6100's starts it by itself, in seconds.
#
set -euo pipefail
RADIO=root@x6200.local
MODE=
YES=
TO=1.0.8
FILE=
UNKNOWN=
TEST=
while [ $# -gt 0 ]; do
    case "$1" in
        --check | --flash | --recover | --dry-run) MODE=$1 ;;
        --yes) YES=1 ;;
        --to) TO=${2:-}; shift ;;
        --file) FILE=${2:-}; shift ;;
        --unknown-ok) UNKNOWN=--unknown-ok ;;
        --test-ok) TEST=--test-ok ;;
        -*) echo "unknown option $1"; exit 1 ;;
        *) RADIO=$1 ;;
    esac
    shift
done
[ -n "$MODE" ] || { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 1; }
TOP=$(cd "$(dirname "$0")/../.." && pwd)
LIST=$TOP/rootfs/usr/share/x6100/base_releases.txt
sha() { shasum -a 256 "$1" 2> /dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }
if [ -n "$FILE" ]; then
    [ -f "$FILE" ] || { echo "no $FILE"; exit 1; }
    F=$FILE
    TO=$(awk -v s="$(sha "$F")" '!/^#/ && $5 == s { print $1; exit }' "$LIST")
    [ -n "$TO" ] || TO="(not in the X6200 list; base_flash identifies it on the radio)"
else
    # The release's files in the list, newest build first; the first here, by its SHA-256
    F=
    for s in $(awk -v v="$TO" '!/^#/ && $1 == v { print $5 }' "$LIST"); do
        for c in "$TOP"/build/base_update/*.xgf; do
            [ -f "$c" ] && [ "$(sha "$c")" = "$s" ] && { F=$c; break 2; }
        done
    done
    [ -n "$F" ] || { echo "no file of base $TO in build/base_update: tools/base_update/fetch.sh $TO"; exit 1; }
fi
XGF=$(basename "$F")
# The name goes into the radio's shell as it is: letters, digits, dot, dash and underscore only
case "$XGF" in *[!A-Za-z0-9._-]*) echo "$XGF: rename it to letters, digits, '.', '-' and '_' only"; exit 1 ;; esac
BIN=$TOP/build/base_update/base_flash
[ -x "$BIN" ] || { echo "no $BIN: run tools/base_update/build.sh first"; exit 1; }
# One connection to the radio for everything here (the log is followed every 2 s): some networks shut port 22 after
# a burst of new connections
mkdir -p ~/.ssh/cm
CM=(-o ControlMaster=auto -o "ControlPath=$HOME/.ssh/cm/%r@%h:%p" -o ControlPersist=10m)
SSH=(ssh "${CM[@]}" -o BatchMode=yes -o ConnectTimeout=10 "$RADIO")
D=/tmp/base_update

"${SSH[@]}" "mkdir -p $D"
scp -q "${CM[@]}" "$BIN" "$F" "$RADIO:$D/"

if [ "$MODE" = --check ]; then
    "${SSH[@]}" "$D/base_flash --check $D/$XGF $UNKNOWN $TEST"
    exit
fi

if [ "$MODE" = --dry-run ]; then
cat << EOF
This asks the base of $RADIO for its bootloader and starts its application again: nothing is erased or written. The
GUI is stopped meanwhile and started again after. The X6100's only.
EOF
else
cat << EOF
This writes the base firmware $TO ($XGF) into the radio's base on $RADIO.
  - Xiegu's conditions: a 13.8 V supply of 3 A or more, the battery out, for the whole write.
  - The base's application is erased and written again. A write that fails after the erase leaves the base without
    one until this is run again with --recover (its bootloader is never erased; not yet tried after a failure).
  - Do not switch the radio off until this says it is done.
EOF
fi
if [ -n "$TEST" ]; then
    echo "  - A TEST BUILD: not validated on a radio. Keep the way back at hand (Xiegu's file for this radio)."
    if [ -z "$YES" ]; then
        read -r -p "Type TEST to write a test build: " answer
        [ "$answer" = TEST ] || { echo "not written"; exit 1; }
    fi
fi
if [ -n "$UNKNOWN" ]; then
    echo "  - $XGF IS NOT ONE OF XIEGU'S FILES KNOWN HERE: it passes the checks an encrypted file allows, but what it"
    echo "    holds can't be told."
    if [ -z "$YES" ]; then
        read -r -p "Type UNKNOWN to write an unknown file: " answer
        [ "$answer" = UNKNOWN ] || { echo "not written"; exit 1; }
    fi
fi
if [ -z "$YES" ]; then
    read -r -p "Type FLASH to go on: " answer
    [ "$answer" = FLASH ] || { echo "not written"; exit 1; }
fi

# On the radio, apart from this session (tools/base_update/run_on_radio.sh): GUI off, the write, the GUI back unless
# the base was left without its application (exit 3), the log kept on the card
EXTRA="$UNKNOWN $TEST"
[ "$MODE" = --recover ] && EXTRA="$EXTRA --recover"
[ "$MODE" = --dry-run ] && EXTRA="$EXTRA --dry-run"
scp -q "${CM[@]}" "$TOP/tools/base_update/run_on_radio.sh" "$RADIO:$D/"
"${SSH[@]}" "rm -f $D/flash.log; setsid sh $D/run_on_radio.sh $D $D/$XGF $EXTRA > /dev/null 2>&1 &"

# Follow the log until its END line, through dropped connections; 20 minutes at most (a normal write takes about one)
shown=0
for _ in $(seq 600); do
    sleep 2
    out=$("${SSH[@]}" "cat $D/flash.log 2> /dev/null" 2> /dev/null) || { echo "(no connection to the radio, trying again)"; continue; }
    total=$(printf '%s\n' "$out" | wc -l)
    if [ "$total" -gt "$shown" ]; then
        printf '%s\n' "$out" | tail -n +"$((shown + 1))"
        shown=$total
    fi
    case "$out" in *"END "*) break ;; esac
done
rc=$(printf '%s\n' "$out" | sed -n 's/^END //p' | tail -1)
[ -n "$rc" ] || { echo "no end of the write seen in 20 minutes: its log is $D/flash.log on the radio"; exit 4; }
if [ "$rc" = 0 ] && [ "$MODE" = --dry-run ]; then
    echo "Dry run done: nothing was erased or written."
elif [ "$rc" = 0 ]; then
    echo "Written. If the radio switches itself off, switch it on again; then $0 $RADIO --check shows the version."
fi
exit "$rc"
