#!/bin/bash
#
# base_flash's tests, on this computer (a Mac or a Linux PC; a C compiler and Python 3): the unit tests (units.c)
# and every scenario of the simulated base (sim.py), the slow ones side by side; about two minutes. Uses Xiegu's base
# files from build/base_update when they are there (tools/base_update/fetch.sh), else a stand-in of the same size
# for the transfers. Work files in build/base_update/test.
#
#     tools/base_update/test/run.sh
#
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$T/../../.." && pwd)
W=$TOP/build/base_update/test
CC=${HOST_CC:-cc}
mkdir -p "$W"
cd "$W" || exit 1
X8=$TOP/build/base_update/x6200_mcu_20260820141208.xgf
X7=$TOP/build/base_update/x6200_mcu_20260116092100.xgf
KNOWN=()
[ -f "$X8" ] && KNOWN+=("$X8")
[ -f "$X7" ] && KNOWN+=("$X7")

"$CC" -O1 -w -I "$T/stub" -o units "$T/units.c" || exit 1
./units "$TOP/rootfs/usr/share/x6100/base_releases.txt" ${KNOWN[@]+"${KNOWN[@]}"} || fails=1

# x6x00-base-fetch on small update zips made as Xiegu's are (synth.py), through Python's zlib and through libz as on
# the radio: a file the list has comes out checked, one it does not comes out unverified with its line for the list
FETCH=$TOP/rootfs/usr/sbin/x6x00-base-fetch
head -c 32768 /dev/urandom > "$W/made_up.xgf"
python3 "$T/synth.py" "$W/synth_new.zip" x6200_mcu_20300101000000.xgf "$W/made_up.xgf"
[ -f "$X8" ] && python3 "$T/synth.py" "$W/synth_108.zip" x6200_mcu_20260820141208.xgf "$X8"
for way in zlib libz; do
    env=()
    [ "$way" = libz ] && env=(X6X00_LIBZ=1)
    rm -rf "$W/fetch_$way"
    if env "${env[@]+"${env[@]}"}" python3 "$FETCH" get --zip "$W/synth_new.zip" --out "$W/fetch_$way" > "$W/fetch_$way.log" 2>&1 \
        && cmp -s "$W/made_up.xgf" "$W/fetch_$way/x6200_mcu_20300101000000.xgf" \
        && grep -q "NOT in the list" "$W/fetch_$way.log" && [ -f "$W/fetch_$way/x6200_mcu_20300101000000.xgf.json" ]; then
        echo "pass  x6x00-base-fetch ($way): a file not in the list comes out whole, unverified"
    else
        echo "FAIL  x6x00-base-fetch ($way), unknown file: $W/fetch_$way.log"; fails=1
    fi
    if [ -f "$X8" ]; then
        if env "${env[@]+"${env[@]}"}" python3 "$FETCH" get --zip "$W/synth_108.zip" --out "$W/fetch_$way" >> "$W/fetch_$way.log" 2>&1 \
            && cmp -s "$X8" "$W/fetch_$way/x6200_mcu_20260820141208.xgf" && grep -q "as the list has it" "$W/fetch_$way.log"; then
            echo "pass  x6x00-base-fetch ($way): Xiegu's 1.0.8 file comes out checked against the list"
        else
            echo "FAIL  x6x00-base-fetch ($way), 1.0.8: $W/fetch_$way.log"; fails=1
        fi
    fi
done
# Told apart by SHA-256 alone: the same file under another name is not kept twice, another file under the same name
# is not overwritten
rm -rf "$W/dedupe" && mkdir -p "$W/dedupe" && cp "$W/made_up.xgf" "$W/dedupe/mine.xgf"
if python3 "$FETCH" get --zip "$W/synth_new.zip" --out "$W/dedupe" > "$W/dedupe.log" 2>&1; grep -q "here already" "$W/dedupe.log" \
    && [ "$(ls "$W/dedupe" | grep -c '\.xgf$')" = 1 ]; then
    echo "pass  x6x00-base-fetch: a file here already under another name is not kept twice"
else
    echo "FAIL  x6x00-base-fetch, same file twice: $W/dedupe.log"; fails=1
fi
rm -rf "$W/clobber" && mkdir -p "$W/clobber" && head -c 32768 /dev/urandom > "$W/clobber/x6200_mcu_20300101000000.xgf"
cp "$W/clobber/x6200_mcu_20300101000000.xgf" "$W/clobber_orig"
SUM=$(shasum -a 256 "$W/made_up.xgf" 2> /dev/null || sha256sum "$W/made_up.xgf")
if python3 "$FETCH" get --zip "$W/synth_new.zip" --out "$W/clobber" > "$W/clobber.log" 2>&1; \
    cmp -s "$W/clobber_orig" "$W/clobber/x6200_mcu_20300101000000.xgf" \
    && cmp -s "$W/made_up.xgf" "$W/clobber/x6200_mcu_20300101000000-$(echo "$SUM" | cut -c1-8).xgf"; then
    echo "pass  x6x00-base-fetch: another file of the same name is kept, the new one beside it"
else
    echo "FAIL  x6x00-base-fetch, same name: $W/clobber.log"; fails=1
fi

# The daily check is off until the owner turns it on (Settings > Info > Daily base check, base_daily_check in
# params.db): no params.db, no row and 0 all answer 3 without asking xiegu.eu; 1 goes on to the check. The network is
# a closed port here (a proxy nobody listens on), so a look that does go out fails, and shows that it went
rm -rf "$W/daily" && mkdir -p "$W/daily"
for v in none norow 0 1; do
    db=$W/daily/params_$v.db
    case $v in
        none) ;;
        norow) python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('CREATE TABLE params (name TEXT PRIMARY KEY ON CONFLICT REPLACE, val INTEGER)'); c.execute(\"INSERT INTO params VALUES ('clock_12h', 1)\"); c.commit()" "$db" ;;
        *) python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('CREATE TABLE params (name TEXT PRIMARY KEY ON CONFLICT REPLACE, val INTEGER)'); c.execute(\"INSERT INTO params VALUES ('base_daily_check', ?)\", (int(sys.argv[2]),)); c.commit()" "$db" "$v" ;;
    esac
    env https_proxy=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 no_proxy= NO_PROXY= \
        python3 "$FETCH" daily --params "$db" --base 1.0.8 --out "$W/daily/out_$v" > "$W/daily/$v.log" 2>&1
    rc=$?
    if [ "$v" = 1 ]; then
        if [ "$rc" = 1 ] && ! grep -q "is off" "$W/daily/$v.log"; then
            echo "pass  x6x00-base-fetch daily: on (1), it goes on to ask xiegu.eu"
        else
            echo "FAIL  x6x00-base-fetch daily, on: exit $rc, $W/daily/$v.log"; fails=1
        fi
    elif [ "$rc" = 3 ] && grep -q "is off" "$W/daily/$v.log" && [ ! -e "$W/daily/out_$v" ]; then
        echo "pass  x6x00-base-fetch daily: off ($v), xiegu.eu not asked"
    else
        echo "FAIL  x6x00-base-fetch daily, off ($v): exit $rc, $W/daily/$v.log"; fails=1
    fi
done

# The transfers: Xiegu's 1.0.8 file, or a stand-in of its size that only the simulated base sees (base_flash takes
# only Xiegu's files: the stand-in is let through by building the scenarios with its hash in place of 1.0.8's)
if [ -f "$X8" ]; then
    F=$X8
    SRC=$T/../base_flash.c
else
    F=$W/standin.xgf
    head -c 524288 /dev/urandom > "$F"
    H=$(shasum -a 256 "$F" 2> /dev/null || sha256sum "$F")
    sed "s/9a19f97a5ad9e141a3663d222f984073bf0b18b38d4662604372a0036f9daa23/${H%% *}/" "$T/../base_flash.c" > "$W/base_flash.c"
    SRC=$W/base_flash.c
    echo "no Xiegu file in build/base_update: the transfers use a stand-in"
fi

pids=()
for mode in ok silent stream dead noerase nostart nak can garbage eot x6100; do
    python3 "$T/sim.py" "$CC" "$SRC" "$T/stub" "$W" "$F" "$mode" > "$W/result_$mode" 2>&1 &
    pids+=($!)
done
[ -f "$X7" ] && { python3 "$T/sim.py" "$CC" "$T/../base_flash.c" "$T/stub" "$W" "$X7" ok ok107 > "$W/result_ok107" 2>&1 & pids+=($!); }

# The X6100 (sim6100.py): Xiegu's V1.1.8 file when build/base_update has it, else a stand-in of its size let through
# the same way, in place of V1.1.8's sum
X18=$TOP/build/base_update/X6100_BBFW_V1.1.8_240915003.xgf
if [ -f "$X18" ]; then
    F6=$X18
    SRC6=$T/../base_flash.c
else
    F6=$W/standin6100.xgf
    head -c 391584 /dev/urandom > "$F6"
    H6=$(shasum -a 256 "$F6" 2> /dev/null || sha256sum "$F6")
    sed "s/8516de0bdcb4a3dcbba1605a482250d17acad2402f89f5d3719db33d6c48348d/${H6%% *}/" "$T/../base_flash.c" > "$W/base_flash6100.c"
    SRC6=$W/base_flash6100.c
    echo "no X6100 file of Xiegu's in build/base_update: the X6100 transfers use a stand-in"
fi
for mode in ok zeros dry nogreet noerase noprog nostart nak eot x6200dt; do
    python3 "$T/sim6100.py" "$CC" "$SRC6" "$T/stub" "$W" "$F6" "$mode" > "$W/result_x6100_$mode" 2>&1 &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || fails=1; done
cat "$W"/result_*
[ -z "${fails:-}" ] && echo "all base_flash tests pass" || { echo "BASE_FLASH TESTS FAILED (logs in $W)"; exit 1; }
