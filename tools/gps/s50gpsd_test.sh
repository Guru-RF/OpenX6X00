#!/bin/sh
# Host test of the ports rootfs/etc/init.d/S50gpsd hands gpsd (#237), and of the hotplug script's wait for it
# (rootfs/etc/default/gpsd): S50gpsd's own functions, its /dev, sysfs and device tree under test.d/s50.
#   sh s50gpsd_test.sh <rootfs>
# Each port once, by the kernel's name: a GPS udev knows (a /dev/gps* link) on ttyACM0 was handed over twice before,
# as /dev/ttyACM0 and /dev/gps0, and gpsd opened it twice. The X6200's WR12 (0483:5740) left out, on the X6200 only.
R=${1:?rootfs folder}
T=$(pwd)/test.d/s50
fails=0
checks=0

# S50gpsd and /etc/default/gpsd with their paths under $T, the functions only (no case at the end)
rm -rf "$T" && mkdir -p "$T/dev" "$T/run" || exit 2
sed -e "s#/var/run/#$T/run/#g" "$R/etc/default/gpsd" > "$T/default-gpsd"
sed -e "s#/etc/default/gpsd#$T/default-gpsd#g" -e "s#/var/run/#$T/run/#g" -e "s#/sys/class/tty#$T/sys/class/tty#g" \
    -e "s#/proc/device-tree/compatible#$T/compatible#g" -e "s# /dev/# $T/dev/#g" \
    -e '/^case "\$1" in/,$d' "$R/etc/init.d/S50gpsd" > "$T/S50gpsd"
grep -q "$T/dev/gps\*" "$T/S50gpsd" || { echo "FAIL S50gpsd's ports not found to put under test.d"; exit 1; }

check() {   # what, want, got
        checks=$((checks + 1))
        if [ "$2" = "$3" ]; then
                echo "ok   $1"
        else
                echo "FAIL $1: \"$3\", not \"$2\""
                fails=$((fails + 1))
        fi
}

# A port: name, USB id (vendor:product), the udev link (gps0...) or nothing
port() {
        : > "$T/dev/$1"
        mkdir -p "$T/sys/class/tty/$1/device"
        echo "${2%:*}" > "$T/sys/class/tty/$1/idVendor"
        echo "${2#*:}" > "$T/sys/class/tty/$1/idProduct"
        [ -n "$3" ] && ln -s "$T/dev/$1" "$T/dev/$3"
}

radio() {   # x6100 or x6200: no ports, the device tree's model
        rm -rf "$T/dev" "$T/sys" && mkdir -p "$T/dev"
        [ "$1" = x6200 ] && printf 'xiegu,x6200\0' > "$T/compatible" || printf 'xiegu,x6100\0' > "$T/compatible"
}

ports() {   # S50gpsd's list, the folder cut off
        (. "$T/S50gpsd"; devices; echo $DEVICES) | sed "s#$T/dev/##g"
}

radio x6100
port ttyACM0 1546:01a7 gps0
check "X6100, a GPS udev knows on ttyACM0: ttyACM0 once, not as gps0 too" "ttyACM0" "$(ports)"

radio x6100
port ttyUSB0 067b:2303 gps0
check "X6100, a GPS udev knows on ttyUSB0: ttyUSB0" "ttyUSB0" "$(ports)"

radio x6100
port ttyACM0 1a86:55d4
check "X6100, a port udev does not know on ttyACM0: ttyACM0, as before" "ttyACM0" "$(ports)"

radio x6100
port ttyACM0 1546:01a8 gps0
port ttyUSB0 10c4:ea60 gps1
check "X6100, two GPS udev knows: each once" "ttyACM0 ttyUSB0" "$(ports)"

radio x6100
port ttyACM0 0483:5740
check "X6100, a 0483:5740 port on ttyACM0: kept (no WR12 on an X6100)" "ttyACM0" "$(ports)"

radio x6200
port ttyACM0 0483:5740
port ttyACM1 1546:01a7 gps1
check "X6200, the WR12 on ttyACM0 and a GPS udev knows on ttyACM1: the GPS only" "ttyACM1" "$(ports)"

radio x6200
port ttyACM0 0483:5740
check "X6200, the WR12 alone: nothing" "" "$(ports)"

radio x6100
check "no port: nothing" "" "$(ports)"

# The hotplug script's wait: USBAUTO=false until S50gpsd has started gpsd
rm -f "$T/run/gpsd.started"
check "the hotplug script before S50gpsd's start: left out" "false" "$(. "$T/default-gpsd"; echo $USBAUTO)"
: > "$T/run/gpsd.started"
check "after it: let through" "" "$(. "$T/default-gpsd"; echo $USBAUTO)"

echo "$checks checks, $fails failed"
[ $fails -eq 0 ] && rm -rf "$T"
[ $fails -eq 0 ]
