#!/bin/bash
# S00adata (rootfs/etc/init.d) on fake cards: sparse card files with the card image's own partition table
# (made here with sfdisk: the same bytes as genimage's, X6100 and X6200 alike), the shims in bin/ for blkid
# (util-linux 2.38's answers, the real probe of the partition's region), parted (the real one on the file),
# mkfs (the real mkfs.vfat at the partition's offset), mount (a link to a folder per filesystem), dd and
# hexdump (a partition's sectors from the card file), sync (logged, with what the system's /mnt and /mnt
# hold then), and the radio's own x6100-starttrace. "old" is v0.3.8's script (from git), "new" the one
# under test.
#   NEW=<script>     another script under test          ONLY="fresh rotated ..."  only those cases
#   KEEP=1           the card files kept in build/runs (sparse, up to 64 GB each; a copy without
#                    --sparse fills the disk): by default they go at the end, or when the run is
#                    stopped (Ctrl-C, kill; not kill -9: then `make clean`), the rest stays
#   TARGET=<buildroot target>: busybox's applets, blkid and hexdump from it (qemu-arm-static), IMGSH=1 its bash too
H=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$H/../.." && pwd)
WT=$REPO
NEW=${NEW:-$REPO/rootfs/etc/init.d/S00adata}
B=$H/build
OLD=${OLD:-$B/S00adata.v038}
RUNS=$B/runs
ONLY=${ONLY:-}
export WT
pass=0 fail=0
fails=

mkdir -p "$RUNS"
[ -n "$KEEP" ] || trap 'rm -f "$RUNS"/*/card.img' EXIT
trap 'exit 130' INT TERM HUP
[ -s "$OLD" ] || git -C "$REPO" show v0.3.8:rootfs/etc/init.d/S00adata > "$OLD" || exit 1
# The card image's first sector: its two partitions, the boot partition and the system's
MBR=$B/mbr.bin
if [ ! -s "$MBR" ]; then
    truncate -s $((1116160 * 512)) "$B/image.img"
    printf 'label: dos\nlabel-id: 0x00000000\n2048,65536,c,*\n67584,1048576,83\n' | sfdisk -q "$B/image.img" || exit 1
    dd if="$B/image.img" of="$MBR" bs=512 count=1 status=none
    rm -f "$B/image.img"
fi
BB=
if [ -n "$TARGET" ]; then
    export BLKID="qemu-arm-static -L $TARGET $TARGET/sbin/blkid" HEXDUMP="qemu-arm-static -L $TARGET $TARGET/usr/bin/hexdump"
    BB=$B/bbbin
    mkdir -p "$BB"
    for a in cat cmp cp cut date dd find grep head ls mkdir mv od rm rmdir sed tail tr wc; do
        printf '#!/bin/sh\nexec qemu-arm-static -L %s %s/bin/busybox %s "$@"\n' "$TARGET" "$TARGET" $a > "$BB/$a"
        chmod +x "$BB/$a"
    done
fi

ok() {
    if eval "$2"; then
        echo "  PASS $1"; pass=$((pass + 1))
    else
        echo "  FAIL $1"; fail=$((fail + 1)); fails="$fails; $SC: $1"
    fi
}

newcard() {
    SC=$1
    export F=$RUNS/$1
    rm -rf "$F"; mkdir -p "$F/dev" "$F/sys_mnt" "$F/sysroot" "$F/fs"
    truncate -s $(($2 * 512)) "$F/card.img"
    dd if="$MBR" of="$F/card.img" bs=512 count=1 conv=notrunc status=none
    : > "$F/dev/mmcblk0"; : > "$F/mounts"; : > "$F/calls.log"
    ln -sfn "$F/sys_mnt" "$F/mnt"
    "$H/bin/partprobe"
    copy "$NEW" new
    copy "$OLD" old
    echo "== $1 ($2 sectors)"
}

# The script with the radio's places swapped for the fake radio's
copy() {
    sed -e 's|/tmp/sysroot/mnt|@SYSMNT@|g' -e 's|/tmp/sysroot|@SYSROOT@|g' -e 's|/tmp/data_fsck.log|@LOG@|g' \
        -e "s|/mnt|$F/mnt|g" -e "s|@SYSMNT@|$F/sysroot/mnt|g" -e "s|@SYSROOT@|$F/sysroot|g" \
        -e "s|/var/lib/x6100/start_trace|$F/sys_trace|g" -e "s|/var/lib/x6100|$F/varlib|g" \
        -e "s|@LOG@|$F/data_fsck.log|g" -e "s|/proc/mounts|$F/mounts|g" -e "s|/dev/mmcblk0|$F/dev/mmcblk0|g" \
        -e "s|/usr/sbin/x6100-starttrace|$H/bin/starttrace|g" -e 's|\[ -b |[ -e |g' -e 's|\[ ! -b |[ ! -e |g' \
        "$1" > "$F/S00adata.$2"
}

# A start: nothing mounted, the kernel's partitions, rcS's trace lines around the script
boot() {
    : > "$F/mounts"; ln -sfn "$F/sys_mnt" "$F/mnt"; rm -f "$F/sysroot/mnt" "$F/pending"
    : > "$F/calls.log"
    "$H/bin/partprobe"
    T=$H/bin/starttrace
    "$T" --boot
    "$T" "S00adata start"
    if [ -n "$IMGSH" ]; then
        PATH=$H/bin:${BB:+$BB:}$PATH qemu-arm-static -L "$TARGET" "$TARGET/bin/bash" ${X:+-x} "$F/S00adata.$1" start > "$F/out.txt" 2>&1
    else
        PATH=$H/bin:${BB:+$BB:}$PATH bash ${X:+-x} "$F/S00adata.$1" start > "$F/out.txt" 2>&1
    fi
    rc=$?
    if [ $rc -eq 0 ]; then "$T" "S00adata end"; else "$T" "S00adata end, exit $rc"; fi
    echo "  [$1 start, exit $rc] $(grep -v -e '^mkfs.fat' -e '^Warning: block count mismatch' -e '^$' "$F/out.txt" | sed "s|$F/||g" | tr '\n' '|')"
}

# The real probe of partition $1's region: field $2
pf() {
    (. "$H/lib.sh"; realprobe "$1"; printf '%s\n' "$plines" | sed -n "s/^$2=//p")
}
nparts() { sfdisk -d "$F/card.img" 2>/dev/null | grep -c "^$F/card.img[0-9]"; }
mounted() { grep -q " $F/mnt " "$F/mounts"; }
data() { readlink "$F/mnt"; }
# The trace lines of the last start (from its boot: line), on DATA or the system partition
lasttrace() {
    for t in "$F/fs/3/app_logs/start_trace.txt" "$F/sys_trace.txt"; do
        [ -f "$t" ] && awk '/: boot: /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$t"
    done
}
called() { grep -q "$1" "$F/calls.log"; }
marked() { [ -e "$F/varlib/data_mounted" ]; }
# What a start that left DATA alone says in data_fsck.log, in the system's /mnt (the Files page)
syslog_says() { grep -q "$1" "$F/sys_mnt/data_fsck.log" 2>/dev/null; }

# A third partition the way S00adata's mkpart makes it (on the card's table only)
mkp3() {
    FREE_INFO=$(parted -sm "$F/card.img" print free | tail -n1 | grep free)
    FREE_ARR=(${FREE_INFO//:/ })
    parted -m -s -a opt "$F/card.img" mkpart primary fat32 ${FREE_ARR[1]} ${FREE_ARR[2]} > /dev/null
}
p3geom() { (. "$H/lib.sh"; geom 3); }
# p3's region from sector 1 on, 32 MiB of it: what a lost boot sector leaves of a FAT32 (backup boot
# sector, FSInfo, both FATs on a 4 GB card)
p3sum() { set -- $(p3geom); dd if="$F/card.img" bs=512 skip=$(($1 + 1)) count=65536 status=none | md5sum; }
# The first $1 sectors of p3 gone (zeros): sector 0 alone, a card's 4 or 16 KiB page, 64 KiB, 4 MiB
zero_p3() { n=$1; set -- $(p3geom); dd if=/dev/zero of="$F/card.img" bs=512 seek=$1 count=$n conv=notrunc status=none; }
# The card written with a new image: the table back to two partitions, a new system partition (no
# trace, no mark, an empty /mnt folder); what lies past the image stays
reimage() {
    dd if="$MBR" of="$F/card.img" bs=512 count=1 conv=notrunc status=none
    rm -rf "$F"/sys_trace*.txt "$F/varlib" "$F/sys_mnt"/* "$F/sys_mnt"/.[!.]* 2>/dev/null
    "$H/bin/partprobe"
}
# The region of the third partition as it was before the new image (no table entry): 32 MiB from sector 1
regionsum() {
    set -- $(sfdisk -d "$F/card.img" 2>/dev/null | sed -n "s|^$F/card.img2 : start= *\([0-9]*\), size= *\([0-9]*\).*|\1 \2|p")
    dd if="$F/card.img" bs=512 skip=$(($1 + $2 + 1)) count=65536 status=none | md5sum
}

# What the GUI writes into the system's /mnt folder on a card running without DATA
gui_writes() {
    d=$F/sys_mnt
    python3 -I "$H/mkwal.py" "$d/params.db"
    sqlite3 "$d/qso_log.db" "create table q(c); insert into q values('DL1ABC')"
    mkdir -p "$d/app_logs" "$d/backups" "$d/themes/mono" "$d/https" "$d/sstv/tx_text" "$d/update" "$d/field/logs"
    echo run > "$d/app_logs/x6100_log_20261101_101500.txt"
    echo bat > "$d/app_logs/battery_20261101.csv"
    echo tgz > "$d/backups/backup.tgz"
    echo '[]' > "$d/channels.json"
    echo '{}' > "$d/themes/mono/theme.json"
    echo pem > "$d/https/cert.pem"
    echo cq > "$d/sstv/tx_text/cq.txt"
    echo 50 > "$d/update/.progress"
    echo adi > "$d/field/logs/a.adi"
    echo h > "$d/.hidden"
}
# The setting only in params.db-wal, read from a copy of folder $1 (with or without the -wal)
walrow() {
    tmp=$(mktemp -d "$RUNS/wal.XXXX")
    cp -a "$1"/params.db* "$tmp/"
    [ "$2" = nowal ] && rm -f "$tmp/params.db-wal" "$tmp/params.db-shm"
    python3 -I -c "import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute(\"select val from params where name='after'\").fetchall())" "$tmp/params.db"
    rm -rf "$tmp"
}
WALROW="[('only_in_wal',)]"
# qso_log.db in folder $1 as SQLite reads it, from a copy with its -journal: integrity_check and the rows
# of table $2
logrows() {
    tmp=$(mktemp -d "$RUNS/log.XXXX")
    cp -a "$1"/qso_log.db* "$tmp/"
    python3 -I -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); print(c.execute('pragma integrity_check').fetchone()[0], c.execute('select count(*) from ' + sys.argv[2]).fetchone()[0])" "$tmp/qso_log.db" "$2" 2>&1 | tail -n 1
    rm -rf "$tmp"
}
# A listing of folder $1 (names, then contents) without the radio's own two files there
tree() { (cd "$1" && find . -path ./app_logs/start_trace.txt -prune -o -path ./data_fsck.log -prune -o -print | sort | md5sum); }
sums() { (cd "$1" && find . -type f ! -path ./app_logs/start_trace.txt ! -path ./data_fsck.log | sort | xargs md5sum | md5sum); }

want() { [ -z "$ONLY" ] || case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

C4G=7744512
C32G=62333952
C64G=124735488

. "$H/cases.sh"

echo
echo "$pass passed, $fail failed$fails"
[ $fail -eq 0 ]
