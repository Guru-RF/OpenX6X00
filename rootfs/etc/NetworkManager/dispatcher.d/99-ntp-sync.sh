#!/bin/sh
# Network up: the clock from NTP, then into the RTC (rtc1, the PCF8563) in UTC, which is how the
# kernel reads it at boot. Without -u hwclock wrote the local time, and every boot without a
# network came up off by the time zone. The pool first, NIST when it does not answer.
#
# In the background: NetworkManager waits for each of these scripts, and with no answer from the
# servers ntpdate takes seconds. A link just up often loses its first packets, or has no DNS
# for a moment: three tries, 10 s apart. One run at a time: a second link coming up meanwhile
# (WiFi and the tether) has nothing to add.

interface=$1 status=$2
[ "$status" = up ] || exit 0

LOCK=/var/run/ntp-sync.lock

sync_clock() {
    flock -n 9 || return 0
    tries=1
    until /usr/bin/ntpdate -u pool.ntp.org || /usr/bin/ntpdate -u time.nist.gov; do
        [ $tries -lt 3 ] || return 1
        tries=$((tries + 1))
        sleep 10
    done
    hwclock -w -u -f /dev/rtc1 && touch /var/run/time-sync
    # ntpd starts at boot (S49ntp), mostly before any network: the server names in ntp.conf did not
    # resolve then, and it asks for them again only minutes later, up to 17. Until then it keeps
    # nothing, and the FT8 window's clock shows as not kept. Started again it finds them at once.
    # Not when it keeps the clock already (from the GPS) or has its servers: then it is left as it is.
    # fd 9 closed for it, or ntpd would hold the lock for good
    if ! ntpq -c "rv 0 leap" 2> /dev/null | grep -q "leap=00" &&
       ! ntpq -pn 2> /dev/null | awk 'NR > 2 && substr($0, 2) !~ /^127\.127\./ { n++ } END { exit !n }'; then
        echo "ntpd has no time server yet: started again"
        /etc/init.d/S49ntp restart 9>&-
    fi
}

sync_clock 9> "$LOCK" < /dev/null >> /var/log/ntpdate.log 2>&1 &
exit 0
