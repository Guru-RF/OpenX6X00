#!/bin/sh
# Host test of rootfs/etc/init.d/S01multiboot, the multiboot card's detection at each start: the kernel's command line
# of our own card on both radios, of a boot menu's systems (x6100mb=, root on another partition), look-alikes, and
# bootmenu.env on the card's first partition (a folder standing in for it, MB_BOOTDIR). The mark it leaves
# (/tmp/x6100-multiboot) goes to a folder of its own here.   sh s01multiboot_test.sh <the script>
S=${1:?the S01multiboot script}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
n=0

# case_ <name> <command line> <bootmenu.env on the first partition: yes/no> <the mark it should leave, "": none>
case_() {
    n=$((n + 1))
    printf '%s\n' "$2" > "$T/cmdline"
    rm -rf "$T/boot" "$T/out"
    mkdir "$T/boot"
    [ "$3" = yes ] && echo "bm_secs=5" > "$T/boot/bootmenu.env"
    echo stale > "$T/out"   # a mark of the start before: gone on a card of its own
    MB_CMDLINE=$T/cmdline MB_BOOTDIR=$T/boot MB_OUT=$T/out sh "$S" start > /dev/null
    got=$(cat "$T/out" 2> /dev/null)
    if [ "$got" = "$4" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1: got '$got', want '$4'"
        fail=1
    fi
}

case_ "X6100, own card"     "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10 fbcon=rotate:3 video=VGA:480x800" no ""
case_ "X6200, own card"     "console=ttyS0 root=/dev/mmcblk0p2 fbcon=rotate:3 video=VGA:480x800" no ""
case_ "menu mark"           "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait x6100mb=1" no "boot menu key F1"
case_ "mark, system 3"      "root=/dev/mmcblk0p6 rootwait x6100mb=3" no "boot menu key F3"
case_ "mark first"          "x6100mb=2 root=/dev/mmcblk0p2" no "boot menu key F2"
case_ "root not p2"         "console=ttyS0,115200 root=/dev/mmcblk0p5 rootwait panic=10" no "root on partition 5"
case_ "root p12"            "root=/dev/mmcblk0p12 rootwait" no "root on partition 12"
case_ "root last"           "console=ttyS0 root=/dev/mmcblk0p7" no "root on partition 7"
case_ "look-alikes"         "root=/dev/mmcblk0p2 nox6100mb=4 myroot=/dev/mmcblk0p5 x6100mb=3a" no ""
case_ "Xiegu's own system"  "console=ttyS0,115200 root=/dev/mmcblk2p2 rootwait" no ""
case_ "menu, system 1"      "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10" yes "boot menu on the card (bootmenu.env)"
case_ "mark before the file" "root=/dev/mmcblk0p2 x6100mb=4" yes "boot menu key F4"

# No command line to read, no first partition: a card of its own
rm -rf "$T/boot" "$T/out"
MB_CMDLINE=$T/none MB_BOOTDEV=$T/none MB_OUT=$T/out sh "$S" start > /dev/null
if [ -e "$T/out" ]; then echo "FAIL nothing to read: a mark left"; fail=1; else echo "ok   nothing to read"; fi
n=$((n + 1))

[ $fail = 0 ] && echo "$n cases, all ok"
exit $fail
