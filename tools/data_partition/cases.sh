# The cases (sourced by run.sh, with its functions and variables)

HINT_BAD="its files are most likely still there: copy them off on a PC with a recovery tool, then format it there as FAT32"
HINT_OTHER="for DATA there, format it as FAT32 on a PC"
HINT_CHECK="check it on a PC (chkdsk /f on Windows); if that does not help, copy its files off there and format it as FAT32"

if want copy; then
    newcard copydiff $C4G
    echo "  the copy differs from the script only in its places:"
    diff "$NEW" "$F/S00adata.new" | grep '^>' | sed "s|$F|<F>|g; s/^/    /" | head -60
fi

# A fresh card's first start: DATA made, formatted, mounted, the card marked
if want fresh; then
    newcard fresh $C4G
    boot new
    ok "three partitions" '[ "$(nparts)" = 3 ]'
    ok "p3 is vfat DATA" '[ "$(pf 3 TYPE)" = vfat ] && [ "$(pf 3 LABEL)" = DATA ]'
    ok "p3 is FAT32" '[ "$(pf 3 VERSION)" = FAT32 ]'
    ok "DATA mounted on /mnt" 'mounted && [ "$(data)" = "$F/fs/3" ]'
    ok "trace: making, formatting, formatted and mounted" 'lasttrace | grep -q "no DATA partition: making it, 571MB to 3965MB" && lasttrace | grep -q "S00adata: formatting $F/dev/mmcblk0p3" && lasttrace | grep -q "formatted as DATA and mounted"'
    ok "trace on DATA from S00adata start on" 'grep -q "S00adata start" "$F/fs/3/app_logs/start_trace.txt" && grep -q "S00adata end" "$F/fs/3/app_logs/start_trace.txt"'
    ok "start ends with exit 0" 'lasttrace | grep -q "S00adata end$"'
    ok "no files moved (system /mnt empty)" '! lasttrace | grep -q "files found"'
    ok "data_fsck.log on DATA" 'grep -q "formatted as DATA" "$F/fs/3/data_fsck.log"'
    ok "the card marked: DATA mounted" 'marked'
    lasttrace | sed "s|$F/||g; s/^/    | /"
    boot new
    ok "2nd start: normal (check, mount)" 'grep -q "Fat32 partition already exists" "$F/out.txt" && called "^fsck.fat" && mounted'
    ok "2nd start: no format" '! called "^mkfs"'
    ok "2nd start: no found_on_system" '[ ! -e "$F/fs/3/found_on_system" ]'
fi

# A new image over a card with a sound DATA: the same DATA, checked and mounted
if want olddata; then
    newcard olddata $C4G
    mkp3; "$H/bin/partprobe"
    PATH=$H/bin:$PATH mkfs -t vfat -n DATA "$F/dev/mmcblk0p3"
    echo old > "$F/fs/3/params.db"; mkdir -p "$F/fs/3/app_logs"; echo l > "$F/fs/3/app_logs/x6100_log_old.txt"
    uuid=$(pf 3 UUID)
    sfdisk -q --delete "$F/card.img" 3
    ok "set up: two partitions, the old DATA still in the free space" '[ "$(nparts)" = 2 ]'
    boot new
    ok "kept, not formatted" 'grep -q "The DATA filesystem of the previous image is still there: kept" "$F/out.txt" && ! called "^mkfs"'
    ok "same filesystem (UUID)" '[ "$(pf 3 UUID)" = "$uuid" ]'
    ok "checked and mounted, the card marked" 'called "^fsck.fat" && mounted && [ "$(cat "$F/fs/3/params.db")" = old ] && marked'
    boot new
    ok "2nd start: normal" 'grep -q "Fat32 partition already exists" "$F/out.txt" && mounted'
fi

# A new image over a third partition formatted on a PC: a FAT32 with another label is DATA at once (the
# next start would take it up all the same); FAT16, which no start takes up, is left alone, said what to do
if want pcreimage; then
    for v in fat32 fat16; do
        newcard "pcreimage${v#fat}" $C4G     # no "fat32" in its path: start's grep reads parted's lines
        mkp3; "$H/bin/partprobe"
        if [ $v = fat32 ]; then
            PATH=$H/bin:$PATH mkfs -t vfat -n VOLUME "$F/dev/mmcblk0p3"
        else
            set -- $(p3geom)
            /usr/bin/mkfs.vfat -F 16 -s 128 -n VOLUME --offset="$1" "$F/card.img" $(($2 / 2)) > /dev/null
        fi
        mkdir -p "$F/fs/3/System Volume Information"; echo mine > "$F/fs/3/notes.txt"
        uuid=$(pf 3 UUID)
        sfdisk -q --delete "$F/card.img" 3; "$H/bin/partprobe"
        boot new
        if [ $v = fat32 ]; then
            ok "fat32: kept as DATA at once, checked, labelled, mounted, the card marked" 'grep -q "A FAT32 filesystem is there, label VOLUME: kept as DATA" "$F/out.txt" && ! called "^mkfs" && called "^fsck.fat" && called "^fatlabel .*p3 DATA" && mounted && marked'
            ok "fat32: the same filesystem, its files as they were" '[ "$(pf 3 UUID)" = "$uuid" ] && [ "$(cat "$F/fs/3/notes.txt")" = mine ]'
            boot new
            ok "fat32, next start: normal" 'grep -q "Fat32 partition already exists" "$F/out.txt" && mounted && ! called "^mkfs"'
        else
            ok "fat16: not formatted, said what to do" '[ "$(pf 3 VERSION)" = FAT16 ] && grep -q "holds another filesystem (vfat): not formatted; $HINT_OTHER" "$F/out.txt" && ! called "^mkfs" && ! mounted'
            boot new
            ok "fat16, next start: not touched (it holds vfat), the same advice" 'grep -q "not FAT32: not touched (it holds vfat); $HINT_OTHER" "$F/out.txt" && ! called "^mkfs" && [ "$(pf 3 UUID)" = "$uuid" ]'
        fi
    done
fi

# A new image over a card whose third partition holds ext4 or NTFS: left alone, and said what to do
for kind in ext4 ntfs; do
    want "$kind" || continue
    newcard "other_$kind" $C4G
    mkp3
    set -- $(p3geom)
    if [ $kind = ext4 ]; then
        mkfs.ext4 -q -F -E offset=$(($1 * 512)) "$F/card.img" $(($2 / 2))k
    else
        python3 -I "$H/mkntfs.py" "$F/card.img" $(($1 * 512)) "$2"
    fi
    uuid=$(pf 3 UUID)
    sfdisk -q --delete "$F/card.img" 3
    boot new
    ok "fresh path: $kind found, not formatted, said what to do" 'grep -q "holds another filesystem ($kind): not formatted; $HINT_OTHER" "$F/out.txt" && ! called "^mkfs"'
    ok "still $kind, same UUID" '[ "$(pf 3 TYPE)" = $kind ] && [ "$(pf 3 UUID)" = "$uuid" ]'
    ok "not mounted, why in data_fsck.log on the system's /mnt" '! mounted && syslog_says "holds another filesystem ($kind)"'
    boot new
    ok "next start: not touched (it holds $kind), said what to do" 'grep -q "not FAT32: not touched (it holds $kind); $HINT_OTHER" "$F/out.txt" && ! called "^mkfs" && lasttrace | grep -q "not touched (it holds $kind); $HINT_OTHER"'
    ok "next start: still $kind" '[ "$(pf 3 TYPE)" = $kind ] && [ "$(pf 3 UUID)" = "$uuid" ]'
    lasttrace | grep S00adata: | sed "s|$F/||g; s/^/    | /"
done

if want handmade; then
    # Third partitions with nothing on them that are not as S00adata makes them
    for v in type83 short four rc8 blkidonly; do
        newcard "hand_$v" $C4G
        mkp3
        set -- $(p3geom)
        case $v in
            type83) sfdisk -q --part-type "$F/card.img" 3 83 ;;
            short)  sfdisk -q --delete "$F/card.img" 3
                    echo "$1,$(($2 - 2097152)),c" | sfdisk -q -a "$F/card.img" ;;
            four)   sfdisk -q --delete "$F/card.img" 3
                    printf '%s\n' "$1,$(($2 - 2097152)),c" "$(($1 + $2 - 2097152)),,83" | sfdisk -q -a "$F/card.img" ;;
            rc8)    printf 'rc=8\n' > "$F/probe_override_3" ;;
            blkidonly) printf 'rc=0\nTYPE=vfat\nVERSION=FAT32\nLABEL=ODD\n' > "$F/probe_override_3" ;;   # parted sees none
        esac
        boot new
        ok "$v: not touched, not formatted" 'grep -q "not FAT32: not touched" "$F/out.txt" && ! called "^mkfs" && ! mounted'
        [ $v = blkidonly ] && ok "blkidonly: a FAT32 to blkid only, check it on a PC" 'grep -q "not touched (a FAT32 to blkid, not to parted); $HINT_CHECK" "$F/out.txt"'
        grep "not touched" "$F/out.txt" | sed "s|$F/||g; s/^/    | /"
    done
    # The fresh path with a probe that cannot tell (two signatures)
    newcard fresh_rc8 $C4G
    printf 'rc=8\n' > "$F/probe_override_3"
    boot new
    ok "fresh, blkid exit 8: not formatted" 'grep -q "holds another filesystem (blkid -p exit 8): not formatted" "$F/out.txt" && ! called "^mkfs"'
    # and with a FAT32 that only blkid reads: not taken up, as the next start would not either
    newcard fresh_blkidonly $C4G
    printf 'rc=0\nTYPE=vfat\nVERSION=FAT32\nLABEL=ODD\n' > "$F/probe_override_3"
    boot new
    ok "fresh, a FAT32 to blkid only: not formatted, not mounted, check it on a PC" 'grep -q "holds a FAT32 to blkid, not to parted: not formatted; $HINT_CHECK" "$F/out.txt" && ! called "^mkfs" && ! called "^fsck.fat" && ! mounted'
    boot new
    ok "fresh, a FAT32 to blkid only, next start: the same" 'grep -q "not touched (a FAT32 to blkid, not to parted); $HINT_CHECK" "$F/out.txt" && ! called "^mkfs" && ! mounted'
fi

# A card a v0.3.8 first start left without DATA (made, never formatted), the radio on the system's /mnt
stuck() {
    newcard "$1" "$2"
    boot old
    ok "old script: the bug (holds another filesystem, not formatted)" 'grep -q "holds another filesystem: not formatted" "$F/out.txt" && [ -z "$(pf 3 TYPE)" ] && ! mounted'
    gui_writes
    boot old
    ok "old script, next start: not touched" 'grep -q "A third partition exists and is not FAT32: not touched" "$F/out.txt" && ! mounted'
    ok "set up: the WAL row needs the -wal" '[ "$(walrow "$F/sys_mnt" nowal)" = "[]" ] && [ "$(walrow "$F/sys_mnt")" = "$WALROW" ]'
    before=$(tree "$F/sys_mnt"); sums=$(sums "$F/sys_mnt"); n=$(ls -A "$F/sys_mnt" | wc -l)
    boot new
    ok "recognised and formatted" 'grep -q "is the DATA partition an earlier start made and never formatted: formatting it" "$F/out.txt" && called "^mkfs"'
    ok "synced with every file on DATA and still on the system, before they go there" 'grep -qx "sync, system /mnt $n, /mnt $n" "$F/calls.log"'
    ok "p3 vfat DATA FAT32, mounted, the card marked" '[ "$(pf 3 TYPE)" = vfat ] && [ "$(pf 3 LABEL)" = DATA ] && [ "$(pf 3 VERSION)" = FAT32 ] && mounted && marked'
    ok "every file on DATA's top level, same names and contents" '[ "$(tree "$F/fs/3")" = "$before" ] && [ "$(sums "$F/fs/3")" = "$sums" ]'
    ok "params.db with its -wal: the last setting there" '[ -f "$F/fs/3/params.db-wal" ] && [ -f "$F/fs/3/params.db-shm" ] && [ "$(walrow "$F/fs/3")" = "$WALROW" ]'
    ok "no found_on_system" '[ ! -e "$F/fs/3/found_on_system" ]'
    ok "system /mnt emptied" '[ -z "$(ls -A "$F/sys_mnt")" ]'
    ok "trace: recognised, formatting, formatted and mounted, files moved" 'lasttrace | grep -q "never formatted: formatting it" && lasttrace | grep -q "S00adata: formatting" && lasttrace | grep -q "formatted as DATA and mounted" && lasttrace | grep -q "files found in the system.s [^ ]*mnt moved to DATA.s top level"'
    ok "this start's trace on DATA, after the GUI logs moved there" 'grep -q "S00adata start" "$F/fs/3/app_logs/start_trace.txt" && [ -f "$F/fs/3/app_logs/x6100_log_20261101_101500.txt" ]'
    ok "data_fsck.log on DATA says it" 'grep -q "moved to DATA.s top level" "$F/fs/3/data_fsck.log"'
    ok "start ends with exit 0" 'lasttrace | grep -q "S00adata end$"'
    lasttrace | grep -e "S00adata" -e boot: | sed "s|$F/||g; s/^/    | /"
    boot new
    ok "next start: normal, nothing moved" 'grep -q "Fat32 partition already exists" "$F/out.txt" && mounted && ! called "^mkfs" && [ ! -e "$F/fs/3/found_on_system" ] && [ -f "$F/fs/3/params.db" ]'
}
want stuck && stuck stuck $C4G
want stuck32 && stuck stuck32 $C32G

# DATA with settings of its own, and a start that ran without it: its files into found_on_system
if want foundsys; then
    newcard foundsys $C4G
    boot new
    echo top > "$F/fs/3/params.db"
    gui_writes
    boot new
    ok "files into found_on_system, DATA's own params.db kept" '[ -f "$F/fs/3/found_on_system/params.db-wal" ] && [ -f "$F/fs/3/found_on_system/app_logs/x6100_log_20261101_101500.txt" ] && [ "$(cat "$F/fs/3/params.db")" = top ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
    ok "data_fsck.log says found_on_system" 'grep -q "moved to found_on_system$" "$F/fs/3/data_fsck.log"'
fi

# Two such starts, a found_on_system on DATA from the first (its params.db perhaps the only copy of
# those settings): the second's files go into found_on_system.2, the first's as they were
if want foundtwice; then
    newcard foundtwice $C4G
    boot new
    echo top > "$F/fs/3/params.db"
    gui_writes
    boot new
    echo weekend > "$F/fs/3/found_on_system/params.db"
    rm -f "$F/fs/3/found_on_system/params.db-wal" "$F/fs/3/found_on_system/params.db-shm"
    first=$(tree "$F/fs/3/found_on_system"); firstsums=$(sums "$F/fs/3/found_on_system")
    gui_writes
    boot new
    ok "the first found_on_system as it was" '[ "$(tree "$F/fs/3/found_on_system")" = "$first" ] && [ "$(sums "$F/fs/3/found_on_system")" = "$firstsums" ] && [ "$(cat "$F/fs/3/found_on_system/params.db")" = weekend ]'
    ok "the second start's files in found_on_system.2, DATA's params.db kept" '[ -f "$F/fs/3/found_on_system.2/params.db-wal" ] && [ "$(walrow "$F/fs/3/found_on_system.2")" = "$WALROW" ] && [ "$(cat "$F/fs/3/params.db")" = top ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
    ok "data_fsck.log says found_on_system.2" 'grep -q "moved to found_on_system.2$" "$F/fs/3/data_fsck.log"'
    gui_writes
    boot new
    ok "a third: found_on_system.3" '[ -f "$F/fs/3/found_on_system.3/qso_log.db" ] && [ "$(cat "$F/fs/3/found_on_system/params.db")" = weekend ]'
fi

# A start that cannot use DATA for another reason says why and what to do, in data_fsck.log on the
# system's /mnt too: DATA not found (FAT32 to parted, nothing to blkid), its mount failing, the
# partition just made without a device
if want failsays; then
    newcard notfound $C4G
    boot new
    printf 'rc=2\n' > "$F/probe_override_3"
    boot new
    ok "not found: said what to do, in data_fsck.log on the system's /mnt too" 'grep -q "DATA partition not found (none labelled DATA, the third not FAT32 to blkid); $HINT_CHECK" "$F/out.txt" && syslog_says "chkdsk" && lasttrace | grep -q "DATA partition not found" && ! mounted && ! called "^mkfs"'
    newcard mountfail $C4G
    boot new
    touch "$F/fail_mount"
    boot new
    ok "mount fails: said what to do, in data_fsck.log on the system's /mnt too" 'lasttrace | grep -q "mount of .* failed; $HINT_CHECK" && syslog_says "mount failed; $HINT_CHECK" && ! mounted && ! called "^mkfs"'
    newcard nodev $C4G
    touch "$F/no_dev_3"
    boot new
    ok "no device for the new partition: not formatted, said what to do" 'grep -q "The new partition .*p3 has no device yet: not formatted; switch the radio off and on again" "$F/out.txt" && syslog_says "off and on again" && ! called "^mkfs"'
    rm -f "$F/no_dev_3"
    boot new
    ok "next start: the never formatted DATA, formatted" 'called "^mkfs" && mounted'
fi

if want nofree; then
    newcard nofree 1116160
    boot new
    ok "no free space: said, nothing made" 'grep -q "No free space available" "$F/out.txt" && [ "$(nparts)" = 2 ] && ! mounted && lasttrace | grep -q "S00adata: No free space available"'
fi

if want mkfsfail; then
    newcard mkfsfail $C4G
    boot old
    gui_writes
    touch "$F/fail_mkfs"
    boot new
    ok "format fails: said, not mounted, system /mnt untouched" 'grep -q "Formatting .* failed" "$F/out.txt" && ! mounted && [ -f "$F/sys_mnt/params.db-wal" ] && lasttrace | grep -q "formatting .* failed"'
    rm -f "$F/fail_mkfs"
    boot new
    ok "next start: formatted, files moved" 'mounted && [ -f "$F/fs/3/params.db-wal" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# The copy onto DATA fails (a file the copy cannot read): the folder made for it goes again, the
# system's files stay; the next start that can copy them moves them
if want copyfail; then
    newcard copyfail $C4G
    boot new
    echo top > "$F/fs/3/params.db"
    gui_writes
    echo s > "$F/sys_mnt/unreadable"; chmod 000 "$F/sys_mnt/unreadable"
    boot new
    ok "copy failed: said, no found_on_system left on DATA, the system's files there" 'grep -q "not all copied onto DATA: left there" "$F/data_fsck.log" && [ ! -e "$F/fs/3/found_on_system" ] && [ -f "$F/sys_mnt/params.db-wal" ] && [ -f "$F/sys_mnt/qso_log.db" ]'
    boot new
    ok "again: still no folder left on DATA" '[ -z "$(ls -d "$F/fs/3"/found_on_system* 2>/dev/null)" ]'
    chmod 600 "$F/sys_mnt/unreadable"
    boot new
    ok "readable again: into found_on_system, the system emptied" '[ -f "$F/fs/3/found_on_system/unreadable" ] && [ -f "$F/fs/3/found_on_system/params.db-wal" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# DATA mounted under v0.3.8, its boot sector lost, then many v0.3.8 starts without it (the radio runs
# on the system's /mnt), then the update: the trace line that told of the mount is gone
if want rotated; then
    for v in sector0 page0 first64k; do
        newcard "rotated_$v" $C4G
        boot old                                   # fresh card under v0.3.8: p3 made, left unformatted
        PATH=$H/bin:$PATH mkfs -t vfat -n VOLUME "$F/dev/mmcblk0p3"    # the PC formats it FAT32
        mkdir -p "$F/fs/3/System Volume Information"
        boot old                                   # v0.3.8 checks and mounts it
        ok "set up: v0.3.8 mounted the PC's FAT32, traced its check" 'mounted && grep -q "S00adata: fsck.fat of" "$F/sys_trace.txt"'
        echo top > "$F/fs/3/params.db"; echo log > "$F/fs/3/qso_log.db"
        n=1; [ $v = page0 ] && n=8; [ $v = first64k ] && n=128
        zero_p3 $n
        sum=$(p3sum)
        i=0
        while [ $i -lt 40 ]; do
            boot old > /dev/null
            for s in $(seq 1 31); do "$H/bin/starttrace" "S$s-some-init-script start"; "$H/bin/starttrace" "S$s-some-init-script end"; done
            for s in 1 2 3 4 5 6 7 8 9 10 11; do "$H/bin/starttrace" "GUI line $s, services and the like"; done
            i=$((i + 1))
        done
        echo "    | system trace after 40 starts: $(wc -c < "$F/sys_trace.txt") + $(wc -c < "$F/sys_trace.1.txt" 2>/dev/null || echo 0) bytes"
        ok "set up: 40 v0.3.8 starts without DATA, its check gone from the system's trace" '! grep -qs "S00adata: fsck.fat of" "$F/sys_trace.txt" "$F/sys_trace.1.txt" && ! marked'
        boot new
        ok "rotated ($v): the damaged DATA is NOT formatted" '! called "^mkfs" && [ "$(p3sum)" = "$sum" ]'
        ok "rotated ($v): says what is left of it and what to do, the card marked" 'grep -q "not touched (a FAT32 whose boot sector went bad, its .* found); $HINT_BAD" "$F/out.txt" && marked'
        grep -e "formatting" -e "not touched" "$F/out.txt" | sed "s|$F/||g; s/^/    | /"
    done
fi

# DATA made and used, its first sectors lost, then the card written with a new image (the table back to
# two partitions, a new system partition: no trace, no mark). Sector 0 alone: its copy in sector 6; a
# card's 4 or 16 KiB page: the first FAT, after mkfs.fat's 32 reserved sectors (64 on a card over 32 GB);
# 64 KiB, or 4 MiB on a 64 GB card (a card's allocation unit): the second FAT, after the first
reimaged() {
    newcard "$1" "$2"
    boot new; echo top > "$F/fs/3/params.db"; echo log > "$F/fs/3/qso_log.db"; boot new
    ok "set up ($1): DATA used" 'mounted && [ -f "$F/fs/3/params.db" ]'
    rsv=$(set -- $(p3geom); dd if="$F/card.img" bs=1 skip=$(($1 * 512 + 14)) count=2 status=none | od -An -tu2 | tr -d ' ')
    zero_p3 "$3"
    reimage
    ok "set up ($1): two partitions, the old DATA region still there" '[ "$(nparts)" = 2 ]'
    sum=$(regionsum)
    boot "$4"
    ok "reimage ($1): the damaged DATA region is NOT formatted" '! called "^mkfs" && [ "$(regionsum)" = "$sum" ]'
    grep -e "formatting" -e "not formatted" -e "kept" -e "making" "$F/out.txt" | sed "s|$F/||g; s/^/    | /"
}
if want reimage; then
    reimaged reimage_old $C4G 1 old
    reimaged reimage_new $C4G 1 new
    ok "reimage_new: says what is left of it and what to do, in data_fsck.log too" 'grep -q "holds a FAT32 whose boot sector went bad, its copy in sector 6 found: not formatted; $HINT_BAD" "$F/out.txt" && syslog_says "copy them off on a PC"'
    boot new
    ok "reimage_new, next start: not touched, the card marked" '! called "^mkfs" && [ "$(regionsum)" = "$sum" ] && grep -q "not touched (a FAT32 whose boot sector went bad, its copy in sector 6 found); $HINT_BAD" "$F/out.txt" && marked'
    reimaged reimage_page0 $C4G 8 new
    ok "reimage_page0: its FAT found" 'grep -q "holds a FAT32 whose boot sector went bad, its FAT found: not formatted; $HINT_BAD" "$F/out.txt"'
    reimaged reimage_16k $C4G 32 new
    reimaged reimage_page0_64g $C64G 8 new
    ok "reimage_page0_64g: mkfs.fat reserved 64 sectors there, its FAT found" '[ "$rsv" = 64 ] && grep -q "its FAT found" "$F/out.txt"'
    reimaged reimage_64k $C4G 128 new
    ok "reimage_64k: the first FAT gone too, the second found" 'grep -q "holds a FAT32 whose boot sector went bad, its FAT found: not formatted; $HINT_BAD" "$F/out.txt"'
    reimaged reimage_4m_64g $C64G 8192 new
    ok "reimage_4m_64g: the second FAT found, 7 MiB in" 'grep -q "its FAT found: not formatted" "$F/out.txt"'
    # 4 MiB lost on a 4 GB card: both FATs' first sectors with it (the second begins 3 MiB in), nothing
    # of the FAT32 left where this looks; with no trace of a mount on this system either, the region is
    # taken for an empty one
    newcard reimage_4m $C4G
    boot new; echo top > "$F/fs/3/params.db"; boot new
    zero_p3 8192; reimage; boot new
    if called "^mkfs"; then
        echo "  NOTE reimage_4m: 4 MiB lost on a 4 GB card, then a new image: formatted (nothing left to recognise; the known limit)"
    else
        echo "  NOTE reimage_4m: 4 MiB lost on a 4 GB card, then a new image: not formatted"
    fi
fi

# exFAT made on a PC on the third partition (its type left at 0x0c), its boot sector lost, a new image
if want exfat; then
    newcard reimage_exfat $C4G
    mkp3
    set -- $(p3geom)
    if command -v mkfs.exfat > /dev/null; then
        dd if=/dev/zero of="$F/p3.img" bs=1M count=0 seek=$(($2 / 2048)) status=none
        mkfs.exfat -L PHOTOS "$F/p3.img" > /dev/null 2>&1
        dd if="$F/p3.img" of="$F/card.img" bs=1M seek=$(($1 / 2048)) conv=notrunc,sparse status=none
        rm -f "$F/p3.img"
        ok "set up: exfat on p3" '[ "$(pf 3 TYPE)" = exfat ]'
        zero_p3 1
        sfdisk -q --delete "$F/card.img" 3; "$H/bin/partprobe"
        sum=$(regionsum)
        boot new
        ok "exFAT whose boot sector went bad: NOT formatted, its copy in sector 12 found" '! called "^mkfs" && [ "$(regionsum)" = "$sum" ] && grep -q "holds an exFAT whose boot sector went bad, its copy in sector 12 found: not formatted" "$F/out.txt"'
    else
        echo "  SKIP no mkfs.exfat"
    fi
fi

# A third partition that once held a FAT32 whose first sector was wiped on purpose (wipefs, a tool):
# what is left of it is taken for damage, never formatted; formatted FAT32 on a PC, it is DATA
if want wiped; then
    newcard wiped $C4G
    mkp3; "$H/bin/partprobe"
    PATH=$H/bin:$PATH mkfs -t vfat -n OLD "$F/dev/mmcblk0p3"
    zero_p3 1
    gui_writes
    boot new
    ok "wiped: not formatted, said what to do, the card marked" '! called "^mkfs" && grep -q "not touched (a FAT32 whose boot sector went bad, its copy in sector 6 found); $HINT_BAD" "$F/out.txt" && marked'
    ok "wiped: why in data_fsck.log on the system's /mnt (the Files page)" 'syslog_says "copy them off on a PC with a recovery tool, then format it there as FAT32"'
    PATH=$H/bin:$PATH mkfs -t vfat -n "NO NAME" "$F/dev/mmcblk0p3"
    boot new
    ok "formatted FAT32 on the PC: DATA, the settings kept on the system on its top level" 'mounted && ! called "^mkfs" && [ "$(walrow "$F/fs/3")" = "$WALROW" ] && [ ! -e "$F/fs/3/found_on_system" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# Bernhard, 2 Nov: DATA damaged and left alone, the radio on the system's /mnt (settings made there),
# then the PC formats p3 FAT32 (label VOLUME, Windows' folder)
if want pc2nov; then
    newcard pc2nov $C4G
    boot new; echo old > "$F/fs/3/params.db"; boot new
    zero_p3 1
    boot new
    ok "set up: damaged DATA left alone" '! called "^mkfs" && ! mounted && grep -q "not touched" "$F/out.txt"'
    gui_writes
    PATH=$H/bin:$PATH mkfs -t vfat -n VOLUME "$F/dev/mmcblk0p3"
    mkdir -p "$F/fs/3/System Volume Information"
    boot new
    ok "pc2nov: PC's FAT32 used as DATA" 'mounted && ! called "^mkfs"'
    ok "pc2nov: the settings made meanwhile on DATA's top level (the radio comes up with them)" '[ -f "$F/fs/3/params.db" ] && [ "$(walrow "$F/fs/3")" = "$WALROW" ] && [ -f "$F/fs/3/qso_log.db" ] && [ -d "$F/fs/3/System Volume Information" ] && [ ! -e "$F/fs/3/found_on_system" ]'
    ok "pc2nov: data_fsck.log on DATA says top level" 'grep -q "moved to DATA.s top level: the settings and logs carry over$" "$F/fs/3/data_fsck.log"'
    ls -A "$F/fs/3" | tr '\n' ' ' | sed 's/^/    | top: /'; echo
fi

# A v0.3.8 card left without DATA, its first start on this version formats DATA and moves the settings and
# QSO log onto it (and off the system); its boot sector is lost before the next start's check: no
# "fsck.fat of" line was ever traced
if want firstrun; then
    for v in sector0 first64k; do
        newcard "firstrun_$v" $C4G
        boot old; gui_writes; boot old
        boot new
        ok "set up: formatted, the system's files on DATA's top level, system emptied" 'called "^mkfs" && [ -f "$F/fs/3/qso_log.db" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
        ok "set up: no check traced yet, the card marked" '! grep -qs "S00adata: fsck.fat of" "$F/sys_trace.txt" "$F/sys_trace.1.txt" && marked'
        n=1; [ $v = first64k ] && n=128
        zero_p3 $n
        sum=$(p3sum)
        boot new
        ok "firstrun ($v): the damaged DATA (the carried-over settings and log) is NOT formatted" '! called "^mkfs" && [ "$(p3sum)" = "$sum" ]'
        grep -e "formatting" -e "not touched" "$F/out.txt" | sed "s|$F/||g; s/^/    | /"
    done
fi

# DATA made and used, then its boot sector gone (sector 0, or the first 64 KiB or 4 MiB as an SD card's
# flash page or allocation unit can lose them at a power cut): blkid and parted see nothing on p3. 4 MiB
# on a 4 GB card takes both FATs' first sectors: the mark tells it apart then
if want damaged; then
    for v in sector0 first64k first4m; do
        newcard "damaged_$v" $C4G
        boot new
        echo top > "$F/fs/3/params.db"; echo log > "$F/fs/3/qso_log.db"
        boot new
        ok "set up: a normal start, its check in the system partition's trace" 'grep -q "S00adata: fsck.fat of" "$F/sys_trace.txt" && mounted'
        n=1; [ $v = first64k ] && n=128; [ $v = first4m ] && n=8192
        zero_p3 $n
        ok "set up: nothing blkid or parted knows on p3 now" '[ -z "$(pf 3 TYPE)" ] && ! parted -m -s "$F/card.img" print | grep -q fat32'
        sum=$(p3sum)
        boot new
        ok "damaged DATA ($v): not formatted, not mounted" '! called "^mkfs" && ! mounted'
        ok "damaged DATA ($v): the rest of it as it was" '[ "$(p3sum)" = "$sum" ]'
        case $v in
            sector0)  why="a FAT32 whose boot sector went bad, its copy in sector 6 found" ;;
            first64k) why="a FAT32 whose boot sector went bad, its FAT found" ;;
            first4m)  why="DATA was mounted at an earlier start, its boot sector gone bad since" ;;
        esac
        ok "damaged DATA ($v): the trace says why and what to do" 'lasttrace | grep -q "not touched ($why); $HINT_BAD"'
        ok "damaged DATA ($v): the same in data_fsck.log on the system's /mnt" 'syslog_says "$HINT_BAD"'
        ok "damaged DATA ($v): the mark kept" 'marked'
        grep "not touched" "$F/out.txt" | sed "s|$F/||g; s/^/    | /"
        gui_writes
        rm -f "$F"/sys_trace*.txt
        boot new
        ok "damaged DATA ($v), the system's trace gone since: still not formatted" '! called "^mkfs" && ! mounted && [ "$(p3sum)" = "$sum" ]'
    done
fi

# The fresh path when blkid -p reads nothing of the new partition (2.38: no line, exit 2, as for an
# I/O error): not taken for an empty partition
if want probefail; then
    newcard probefail $C4G
    touch "$F/probe_fail_3"
    boot new
    ok "fresh, blkid -p answers nothing (exit 2): not formatted" '! called "^mkfs" && grep -q "holds another filesystem (blkid -p exit 2): not formatted" "$F/out.txt"'
    gui_writes
    boot new
    ok "next start, still no answer: not touched" '! called "^mkfs" && grep -q "not touched (it holds blkid -p exit 2)" "$F/out.txt"'
    rm -f "$F/probe_fail_3"
    boot new
    ok "next start, blkid answers: the never formatted DATA, formatted, files on top" 'called "^mkfs" && mounted && [ -f "$F/fs/3/params.db-wal" ] && [ ! -e "$F/fs/3/found_on_system" ]'
fi

# The start that formatted DATA cut short in its copy (screen still dark: a power cut): DATA holds
# found_on_system with part of the files, the system's /mnt all of them
if want resume; then
    newcard resume $C4G
    boot old; gui_writes; boot old
    before=$(tree "$F/sys_mnt"); sums=$(sums "$F/sys_mnt")
    PATH=$H/bin:$PATH mkfs -t vfat -n DATA "$F/dev/mmcblk0p3"
    mkdir -p "$F/fs/3/found_on_system"
    cp -a "$F/sys_mnt/params.db" "$F/sys_mnt/app_logs" "$F/fs/3/found_on_system/"
    head -c 10 "$F/sys_mnt/qso_log.db" > "$F/fs/3/found_on_system/qso_log.db"
    boot new
    ok "resumed: normal path" 'grep -q "Fat32 partition already exists" "$F/out.txt" && mounted && ! called "^mkfs"'
    ok "resumed: every file on DATA's top level, same names and contents" '[ "$(tree "$F/fs/3")" = "$before" ] && [ "$(sums "$F/fs/3")" = "$sums" ]'
    ok "resumed: the setting only in the -wal" '[ "$(walrow "$F/fs/3")" = "$WALROW" ]'
    ok "resumed: no found_on_system, system /mnt empty" '[ ! -e "$F/fs/3/found_on_system" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
    ok "resumed: data_fsck.log says top level" 'grep -q "moved to DATA.s top level" "$F/fs/3/data_fsck.log"'
fi

# Cut short later, after some moves: DATA has its own params.db then, the rest goes to found_on_system.2
# and nothing on DATA is replaced
if want resume_moved; then
    newcard resume_moved $C4G
    boot old; gui_writes; boot old
    PATH=$H/bin:$PATH mkfs -t vfat -n DATA "$F/dev/mmcblk0p3"
    mkdir -p "$F/fs/3/found_on_system"
    cp -a "$F/sys_mnt/." "$F/fs/3/found_on_system/"
    mv "$F/fs/3/found_on_system/params.db" "$F/fs/3/found_on_system/params.db-shm" "$F/fs/3/found_on_system/params.db-wal" "$F/fs/3/"
    echo mine > "$F/fs/3/params.db-shm"
    first=$(tree "$F/fs/3/found_on_system")
    boot new
    ok "moved part: DATA's top level kept, its found_on_system as it was, the rest in found_on_system.2, system emptied" '[ "$(cat "$F/fs/3/params.db-shm")" = mine ] && [ "$(tree "$F/fs/3/found_on_system")" = "$first" ] && [ -f "$F/fs/3/found_on_system.2/qso_log.db" ] && [ ! -e "$F/fs/3/qso_log.db" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# DATA that holds nothing but a found_on_system of older files (up to v0.3.8 a start cut short after
# its copy left one, perhaps the only copy of those settings), then a start without DATA: that
# found_on_system as it was; the newer files on DATA's top level through found_on_system.2
if want oldfound; then
    newcard oldfound $C4G
    boot old; gui_writes; boot old
    PATH=$H/bin:$PATH mkfs -t vfat -n DATA "$F/dev/mmcblk0p3"
    mkdir -p "$F/fs/3/found_on_system/app_logs"
    echo weekend > "$F/fs/3/found_on_system/params.db"
    echo older > "$F/fs/3/found_on_system/app_logs/x6100_log_20261031_120000.txt"
    first=$(tree "$F/fs/3/found_on_system"); firstsums=$(sums "$F/fs/3/found_on_system")
    boot new
    ok "older found_on_system: as it was" '[ "$(tree "$F/fs/3/found_on_system")" = "$first" ] && [ "$(sums "$F/fs/3/found_on_system")" = "$firstsums" ]'
    ok "older found_on_system: the system's files on DATA's top level, no found_on_system.2 left, system emptied" '[ "$(walrow "$F/fs/3")" = "$WALROW" ] && [ -f "$F/fs/3/qso_log.db" ] && [ ! -e "$F/fs/3/found_on_system.2" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# p3 formatted FAT32 on a PC (empty, no DATA label), the radio's settings on the system: those on its
# top level, the radio comes up with them
if want pcformat; then
    newcard pcformat $C4G
    boot old; gui_writes; boot old
    before=$(tree "$F/sys_mnt"); sums=$(sums "$F/sys_mnt")
    PATH=$H/bin:$PATH mkfs -t vfat -n "NO NAME" "$F/dev/mmcblk0p3"
    boot new
    ok "PC's FAT32: used as DATA, every file on its top level" 'mounted && [ "$(tree "$F/fs/3")" = "$before" ] && [ "$(sums "$F/fs/3")" = "$sums" ] && [ ! -e "$F/fs/3/found_on_system" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# DATA without params.db but with files of its own: those stay as they are, only the names it lacks
# come up, the rest in found_on_system
if want ownfiles; then
    newcard ownfiles $C4G
    boot old; gui_writes; boot old
    PATH=$H/bin:$PATH mkfs -t vfat -n "NO NAME" "$F/dev/mmcblk0p3"
    echo mylog > "$F/fs/3/qso_log.db"; mkdir -p "$F/fs/3/app_logs"; echo mine > "$F/fs/3/app_logs/x6100_log_20261101_101500.txt"
    boot new
    ok "own files: DATA's qso_log.db and app_logs as they were" '[ "$(cat "$F/fs/3/qso_log.db")" = mylog ] && [ "$(cat "$F/fs/3/app_logs/x6100_log_20261101_101500.txt")" = mine ]'
    ok "own files: the system's in found_on_system, params.db with its -wal on top" '[ "$(sqlite3 "$F/fs/3/found_on_system/qso_log.db" "select c from q")" = DL1ABC ] && [ -f "$F/fs/3/found_on_system/app_logs/battery_20261101.csv" ] && [ "$(walrow "$F/fs/3")" = "$WALROW" ] && [ -f "$F/fs/3/channels.json" ]'
    ok "own files: data_fsck.log says which went where" 'grep -q "moved to DATA.s top level: the settings and logs carry over (a name DATA had already: in found_on_system)" "$F/fs/3/data_fsck.log"'
fi

# DATA without params.db but with a -wal of its own (SQLite would read it into a params.db put beside
# it): the settings files are DATA's, the system's into found_on_system
if want straywal; then
    newcard straywal $C4G
    boot old; gui_writes; boot old
    PATH=$H/bin:$PATH mkfs -t vfat -n "NO NAME" "$F/dev/mmcblk0p3"
    echo stray > "$F/fs/3/params.db-wal"
    boot new
    ok "a -wal of its own: kept, nothing next to it, the system's files in found_on_system" '[ "$(cat "$F/fs/3/params.db-wal")" = stray ] && [ ! -e "$F/fs/3/params.db" ] && [ "$(walrow "$F/fs/3/found_on_system")" = "$WALROW" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
fi

# DATA formatted on a PC, a database of its own there or a journal of one (the QSO log copied back from
# the old DATA, a -journal left by a power cut), the system's qso_log.db with a hot -journal (a power cut
# in a change): those two stay together in found_on_system, DATA's as they were
if want journal; then
    for v in ownlog strayjournal; do
        newcard "journal_$v" $C4G
        boot old; gui_writes; boot old
        python3 -I "$H/mkjournal.py" "$F/sys_mnt/qso_log.db"
        ok "set up ($v): the system's qso_log.db with a hot -journal" '[ -s "$F/sys_mnt/qso_log.db-journal" ] && [ "$(logrows "$F/sys_mnt" qsos)" = "ok 200" ]'
        PATH=$H/bin:$PATH mkfs -t vfat -n "NO NAME" "$F/dev/mmcblk0p3"
        if [ $v = ownlog ]; then
            python3 -I -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('create table mine(call text, pad text)'); [c.execute('insert into mine values(?, ?)', ('MINE%d' % i, 'm' * 400)) for i in range(300)]; c.commit()" "$F/fs/3/qso_log.db"
            cp "$F/fs/3/qso_log.db" "$F/mine.db"
        else
            echo stray > "$F/fs/3/qso_log.db-journal"
        fi
        boot new
        if [ $v = ownlog ]; then
            ok "own log: DATA's qso_log.db as it was, no -journal beside it, SQLite reads it" 'cmp -s "$F/fs/3/qso_log.db" "$F/mine.db" && [ ! -e "$F/fs/3/qso_log.db-journal" ] && [ "$(logrows "$F/fs/3" mine)" = "ok 300" ]'
        else
            ok "stray journal: as it was, no qso_log.db beside it" '[ "$(cat "$F/fs/3/qso_log.db-journal")" = stray ] && [ ! -e "$F/fs/3/qso_log.db" ]'
        fi
        ok "$v: the system's qso_log.db with its -journal in found_on_system, SQLite reads it" '[ -s "$F/fs/3/found_on_system/qso_log.db-journal" ] && [ "$(logrows "$F/fs/3/found_on_system" qsos)" = "ok 200" ]'
        ok "$v: params.db with its -wal on top, the system emptied" '[ "$(walrow "$F/fs/3")" = "$WALROW" ] && [ -z "$(ls -A "$F/sys_mnt")" ]'
    done
fi

# The normal start (FAT32 DATA there): the same tool calls as v0.3.8's script, in the same order
if want samepath; then
    newcard samepath $C4G
    boot new
    echo top > "$F/fs/3/params.db"
    boot old
    cp "$F/calls.log" "$F/calls_old.txt"
    boot new
    ok "normal start: the same tool calls" 'diff "$F/calls_old.txt" "$F/calls.log" > /dev/null'
    gui_writes
    boot old
    cp "$F/calls.log" "$F/calls_old2.txt"
    gui_writes
    boot new
    ok "start with files left on the system (the existing case): the same tool calls, and a sync" 'diff "$F/calls_old2.txt" <(grep -v "^sync" "$F/calls.log") > /dev/null && called "^sync, system /mnt [1-9]"'
fi
