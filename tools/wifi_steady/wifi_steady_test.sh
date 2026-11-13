#!/bin/sh
# 20-wifi-steady against a real wpa_supplicant, run by the Makefile in namespaces of its own (unshare -rmn).
# The supplicant runs a network on a dummy link with the wired driver (no WiFi here), with the background scans
# NetworkManager gives every network. /sys/class/net is a tmpfs on which the dummy link looks like a WiFi device.
#
# 1. A WiFi link up: the network's bgscan is empty after, the supplicant stopped its module, power saving off asked.
# 2. The same link going down: nothing asked.
# 3. Another link up (no WiFi device): nothing asked.
# 4. A WiFi link the supplicant does not have: said in the log, no error.
HOOK=$(realpath "$1")
[ "$(id -u)" = 0 ] && [ "$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ')" = lo ] ||
    { echo "run it with make (unshare -rmn): a network namespace of its own"; exit 2; }
for p in wpa_supplicant gdbus dbus-daemon ip; do
    command -v $p > /dev/null || { echo "$p not found on this PC"; exit 2; }
done
mount -t tmpfs tmpfs /run
W=$(mktemp -d /run/wifi-steady.XXXXXX)
mkdir "$W/bin"
for p in iw logger; do
    printf '#!/bin/sh\necho "%s $*" >> %s/asked\n' $p "$W" > "$W/bin/$p"
    chmod +x "$W/bin/$p"
done
cat > "$W/bus.conf" << CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path=/run/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_type="method_call"/>
<allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>
CONF
ip link add dum0 type dummy && ip link set dum0 up
dbus-daemon --config-file="$W/bus.conf" --nofork --nopidfile & BUS=$!
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/bus
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S /run/bus ] && break; sleep 0.2; done
wpa_supplicant -u -dd > "$W/supplicant.log" 2>&1 & SUPP=$!
trap 'kill $SUPP $BUS 2> /dev/null' EXIT

SUP=fi.w1.wpa_supplicant1
call() { gdbus call --system --dest $SUP "$@"; }
path() { sed -n "s/.*objectpath '\([^']*\)'.*/\1/p"; }
for i in 1 2 3 4 5 6 7 8 9 10; do call --object-path /fi/w1/wpa_supplicant1 --method org.freedesktop.DBus.Peer.Ping \
    > /dev/null 2>&1 && break; sleep 0.3; done
IFP=$(call --object-path /fi/w1/wpa_supplicant1 --method $SUP.CreateInterface \
    "{'Ifname': <'dum0'>, 'Driver': <'wired'>}" | path)
NET=$(call --object-path "$IFP" --method $SUP.Interface.AddNetwork \
    "{'ssid': <'HomeNet'>, 'key_mgmt': <'NONE'>, 'bgscan': <'simple:30:-70:86400'>}" | path)
call --object-path "$IFP" --method $SUP.Interface.SelectNetwork "$NET" > /dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do
    call --object-path "$IFP" --method org.freedesktop.DBus.Properties.Get $SUP.Interface State | grep -q completed \
        && break
    sleep 0.3
done
bgscan() {
    call --object-path "$NET" --method org.freedesktop.DBus.Properties.Get $SUP.Network Properties | tr ',' '\n' |
        sed -n "s/.*'bgscan': <'\(.*\)'>.*/\1/p"
}

mount -t tmpfs tmpfs /sys/class/net
mkdir /sys/class/net/dum0 /sys/class/net/eth0 /sys/class/net/wlan1
touch /sys/class/net/dum0/phy80211 /sys/class/net/wlan1/phy80211

fails=0
check() {   # what, wanted, got
    if [ "$2" = "$3" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1: wanted [$2], got [$3]"
        fails=$((fails + 1))
    fi
}
run() {     # interface, action: what the hook asked of iw and logger
    rm -f "$W/asked"
    PATH="$W/bin:$PATH" sh "$HOOK" "$1" "$2"
    echo "exit $?" >> "$W/asked"
    tr '\n' '|' < "$W/asked"
}

check "background scans at the start" '"simple:30:-70:86400"' "$(bgscan)"
check "1. WiFi up" "iw dev dum0 set power_save off|logger -t wifi dum0: no background scans while connected|exit 0|" \
    "$(run dum0 up)"
check "1. bgscan after" '""' "$(bgscan)"
check "1. the supplicant stopped them" yes \
    "$(grep -q "bgscan: Deinitializing module 'simple'" "$W/supplicant.log" && echo yes)"
check "2. WiFi down" "exit 0|" "$(run dum0 down)"
check "3. another link up" "exit 0|" "$(run eth0 up)"
check "4. a WiFi link the supplicant does not have" \
    "iw dev wlan1 set power_save off|logger -t wifi wlan1: background scans not turned off (no network joined in the supplicant)|exit 0|" \
    "$(run wlan1 up)"
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
