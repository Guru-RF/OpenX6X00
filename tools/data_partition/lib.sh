# Shared by the shims: F (the fake radio's root), CARD (the card file), DEVP (the card's device path)
: "${F:?F unset}"
# the host's tools for the shims themselves, whatever the script under test runs with
PATH=/usr/bin:$PATH
CARD=$F/card.img
DEVP=$F/dev/mmcblk0
PART_UUID_BASE=9c9f3471
# The real probe and hexdump: the host's, or (TARGET) the image's own util-linux 2.38 under qemu
BLKID=${BLKID:-/usr/bin/blkid}
HEXDUMP=${HEXDUMP:-/usr/bin/hexdump}

# "start size type" of partition $1 from the card's table (sectors, hex type without 0x)
geom() {
    sfdisk -d "$CARD" 2>/dev/null | sed -n "s|^${CARD}$1 : start= *\([0-9]*\), size= *\([0-9]*\), type= *\([0-9a-fA-F]*\).*|\1 \2 \3|p"
}

# Partition number of a device path, empty when it is not one of the card's
partnum() {
    case "$1" in
        "$DEVP"p[0-9]*) echo "${1#"$DEVP"p}" ;;
    esac
}

# The real blkid's low-level probe of the region of partition $1 (no DEVNAME line); its exit in prc.
# An override file $F/probe_override_N: its first line "rc=N", the rest the lines blkid prints
realprobe() {
    if [ -f "$F/probe_override_$1" ]; then
        prc=$(sed -n '1s/^rc=//p' "$F/probe_override_$1")
        plines=$(sed -n '2,$p' "$F/probe_override_$1")
        return
    fi
    set -- $(geom "$1")
    [ -n "$1" ] || { prc=2; plines=; return; }
    plines=$($BLKID -p -o export -O $(($1 * 512)) -S $(($2 * 512)) "$CARD" 2>/dev/null)
    prc=$?
    plines=$(printf '%s\n' "$plines" | grep -v -e '^DEVNAME=' -e '^$')
}

callog() {
    echo "$*" >> "$F/calls.log"
}
