#!/bin/sh
# Network up: the clock from NTP, then into the RTC (rtc1, the PCF8563) in UTC, which is how the
# kernel reads it at boot. Without -u hwclock wrote the local time, and every boot without a
# network came up off by the time zone. The pool first, NIST when it does not answer.

interface=$1 status=$2
case "$status" in
  up)
    { /usr/bin/ntpdate -u pool.ntp.org || /usr/bin/ntpdate -u time.nist.gov; } >> /var/log/ntpdate.log 2>&1 && \
    hwclock -w -u -f /dev/rtc1 >> /var/log/ntpdate.log 2>&1 && \
    touch /var/run/time-sync
    ;;
esac
