#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# OpenX6X00 - host test of the USB Bluetooth dongle's scripts (from OlivLoze's fork, F4OLI):
#   rootfs/usr/sbin/bt-dongle against a stand-in sysfs (the radio's own chip on the MUSB host, its interfaces bound to
#   btusb and its WiFi to another driver; a dongle on the EHCI host): which adapter, a dongle or not, what apply lets go
#   and what removed and release bind again, the setting off (as for every radio until it is turned on) and the X6200;
#   rootfs/usr/sbin/btaudio-ctl sync against a stand-in pactl: the headset backend, the cards' profiles, the mic for the
#   GUI and the loopback to a hands-free headset, with and without a dongle in use.
# Nothing here touches the PC's own Bluetooth or PulseAudio: every command the scripts run is a stand-in in
# build/bin. See the Makefile.
#
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
BT_DONGLE = os.path.join(TOP, "rootfs/usr/sbin/bt-dongle")
BTAUDIO_CTL = os.path.join(TOP, "rootfs/usr/sbin/btaudio-ctl")
BUILD = os.path.join(HERE, "build")
BIN = os.path.join(BUILD, "bin")

checks = 0
failed = 0


def check(ok, what):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("FAIL " + what)


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return None


# Stand-ins: logger (the PC's syslog stays out of it), pactl and hcitool for btaudio-ctl, each call in calls.log
def stubs():
    write(os.path.join(BIN, "logger"), "#!/bin/sh\necho \"logger $*\" >> \"$STUB/calls.log\"\n", 0o755)
    write(os.path.join(BIN, "hcitool"), "#!/bin/sh\nexit 0\n", 0o755)
    write(os.path.join(BIN, "pactl"), """#!/bin/sh
echo "pactl $*" >> "$STUB/calls.log"
case "$*" in
	"list short modules") [ -f "$STUB/no-pulse" ] && exit 1; cat "$STUB/modules" 2>/dev/null;;
	"list short sinks")   cat "$STUB/sinks" 2>/dev/null;;
	"list short sources") cat "$STUB/sources" 2>/dev/null;;
	"list cards")         cat "$STUB/cards" 2>/dev/null;;
esac
exit 0
""", 0o755)
    write(os.path.join(BIN, "dongle-yes"), "#!/bin/sh\n[ \"$1\" = dongle ]\n", 0o755)
    write(os.path.join(BIN, "dongle-no"), "#!/bin/sh\nexit 1\n", 0o755)


# The stand-in sysfs
OWN_DEV = "devices/platform/soc/1c19000.usb/musb-hdrc.1.auto/usb2/2-1"
DONGLE_DEV = "devices/platform/soc/1c1a000.usb/usb1/1-1"


class Sys:
    def __init__(self, root):
        self.root = root
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(os.path.join(root, "class/bluetooth"))
        drivers = os.path.join(root, "bus/usb/drivers")
        for d in ("btusb", "rtl8xxxu"):
            os.makedirs(os.path.join(drivers, d))
        write(os.path.join(drivers, "btusb/bind"), "")
        write(os.path.join(drivers, "btusb/unbind"), "")

    def adapter(self, hci, dev, ifaces):
        """hci on the USB device dev, its interfaces {name: driver}; the first one carries the adapter"""
        first = None
        for iface, driver in ifaces.items():
            path = os.path.join(self.root, dev, iface)
            os.makedirs(path, exist_ok=True)
            if not os.path.islink(os.path.join(path, "driver")):     # the chip back after it was let go
                os.symlink(os.path.relpath(os.path.join(self.root, "bus/usb/drivers", driver), path),
                           os.path.join(path, "driver"))
            first = first or path
        os.makedirs(os.path.join(first, "bluetooth", hci))
        os.symlink(os.path.relpath(os.path.join(first, "bluetooth", hci), os.path.join(self.root, "class/bluetooth")),
                   os.path.join(self.root, "class/bluetooth", hci))

    def own(self, hci="hci0"):
        self.adapter(hci, OWN_DEV, {"2-1:1.0": "btusb", "2-1:1.1": "btusb", "2-1:1.2": "rtl8xxxu"})

    def dongle(self, hci="hci1"):
        self.adapter(hci, DONGLE_DEV, {"1-1:1.0": "btusb", "1-1:1.1": "btusb"})

    def gone(self, hci):
        os.unlink(os.path.join(self.root, "class/bluetooth", hci))

    def connection(self, name):
        """an adapter's connection, as hci1:12: not an adapter"""
        os.makedirs(os.path.join(self.root, "devices/virtual/bluetooth", name))
        os.symlink(os.path.relpath(os.path.join(self.root, "devices/virtual/bluetooth", name),
                                   os.path.join(self.root, "class/bluetooth")),
                   os.path.join(self.root, "class/bluetooth", name))


def env(extra=None):
    e = {"PATH": BIN + ":/usr/bin:/bin", "STUB": BUILD, "LC_ALL": "C",
         "BT_DONGLE_SYS": os.path.join(BUILD, "sys"),
         "BT_DONGLE_CONF": os.path.join(BUILD, "btcat.conf"),
         "BT_DONGLE_DEFAULT": os.path.join(BUILD, "default-btcat"),
         "BT_DONGLE_DT": os.path.join(BUILD, "compatible"),
         "BT_DONGLE_UNBOUND": os.path.join(BUILD, "bt-dongle.unbound")}
    e.update(extra or {})
    return e


def dongle(*args):
    r = subprocess.run(["sh", BT_DONGLE] + list(args), env=env(), capture_output=True, text=True, timeout=20)
    return r.returncode, r.stdout.strip()


def setting(on):
    write(os.path.join(BUILD, "btcat.conf"), 'BTCAT_BT="yes"\nBTCAT_DONGLE="%s"\n' % ("yes" if on else "no"))


def fresh():
    for f in ("btcat.conf", "default-btcat", "compatible", "bt-dongle.unbound", "calls.log"):
        p = os.path.join(BUILD, f)
        if os.path.exists(p):
            os.unlink(p)
    write(os.path.join(BUILD, "compatible"), "xiegu,x6100\0allwinner,sun8i-r16\0")
    return Sys(os.path.join(BUILD, "sys"))


def wait_gone(path, s=4.0):
    end = time.time() + s
    while time.time() < end and os.path.exists(path):
        time.sleep(0.1)
    return not os.path.exists(path)


def test_bt_dongle():
    unbound = os.path.join(BUILD, "bt-dongle.unbound")
    unbind = os.path.join(BUILD, "sys/bus/usb/drivers/btusb/unbind")
    bind = os.path.join(BUILD, "sys/bus/usb/drivers/btusb/bind")

    # Every radio as it is: the setting not there, the chip alone
    s = fresh()
    s.own()
    check(dongle() == (0, "hci0"), "no setting, the chip alone: hci0")
    check(dongle("dongle")[0] != 0, "no setting: no dongle")
    check(dongle("apply") == (0, "") and read(unbind) == "" and read(unbound) is None,
          "no setting: apply lets nothing go")

    # A dongle plugged in, the setting off: hci0 still, nothing let go
    s.dongle()
    setting(False)
    check(dongle() == (0, "hci0"), "setting off, chip hci0 and dongle hci1: hci0, as always")
    check(dongle("dongle")[0] != 0, "setting off: no dongle in use")
    check(dongle("apply") == (0, "") and read(unbind) == "", "setting off: apply lets nothing go")
    check(dongle("release") == (0, "") and read(bind) == "", "setting off: release binds nothing")

    # /etc/default/btcat says yes, the user's file no: the user's
    write(os.path.join(BUILD, "default-btcat"), "BTCAT_DONGLE=yes\n")
    check(dongle() == (0, "hci0") and dongle("dongle")[0] != 0, "the user's no over the image's yes: off")
    os.unlink(os.path.join(BUILD, "default-btcat"))

    # The setting on: the dongle, and apply lets the chip's Bluetooth go, its WiFi kept
    setting(True)
    s.connection("hci1:12")
    check(dongle() == (0, "hci1"), "setting on: the dongle hci1 (a connection hci1:12 is not an adapter)")
    check(dongle("dongle")[0] == 0, "setting on: a dongle in use")
    rc, out = dongle("apply")
    check(rc == 0 and "hci1" in out and "hci0" in out, "apply says what it let go: " + out)
    check(read(unbound) == "2-1:1.0\n2-1:1.1\n", "apply: the chip's two btusb interfaces let go, not its WiFi: %r"
          % read(unbound))
    check(read(unbind) == "2-1:1.1\n", "apply wrote to btusb's unbind")

    # The chip gone from the list as btusb let it go; another adapter going while the dongle stays: nothing bound
    s.gone("hci0")
    check(dongle() == (0, "hci1"), "the chip let go: the dongle alone")
    check(dongle("removed") == (0, ""), "removed returns at once")
    time.sleep(1.6)
    check(read(unbound) is not None and read(bind) == "", "removed with the dongle still there: nothing bound")

    # The dongle unplugged: the chip's interfaces bound again, in the background
    s.gone("hci1")
    check(dongle() == (0, ""), "nothing left: no name")
    dongle("removed")
    check(wait_gone(unbound), "removed, no dongle left: the list of what was let go is done with")
    check(read(bind) == "2-1:1.1\n", "removed: the interfaces bound to btusb again")
    calls = read(os.path.join(BUILD, "calls.log")) or ""
    check("logger" in calls, "removed: the log says the radio's own Bluetooth is back")

    # The chip back (its add), as hci0 again, the setting on and no dongle: the chip
    s.own("hci2")
    check(dongle() == (0, "hci2") and dongle("dongle")[0] != 0, "the chip back as hci2, no dongle: the chip")
    check(dongle("apply") == (0, ""), "the chip alone: apply lets nothing go")

    # release: the setting turned off with the dongle in use
    s = fresh()
    s.dongle("hci0")
    s.own("hci1")
    setting(True)
    check(dongle() == (0, "hci0"), "a dongle that came first (hci0), the chip after it (hci1): the dongle")
    dongle("apply")
    check(read(unbound) == "2-1:1.0\n2-1:1.1\n", "the chip as hci1 let go the same way")
    setting(False)
    write(bind, "")
    check(dongle("release") == (0, "") and read(unbound) is None and read(bind) == "2-1:1.1\n",
          "release: bound again at once")
    check(dongle() == (0, "hci0"), "setting off with the dongle as hci0: hci0 (the dongle as the only adapter)")

    # No adapter at all
    s = fresh()
    setting(True)
    check(dongle() == (0, "") and dongle("dongle")[0] != 0, "setting on, no adapter: none")
    setting(False)
    check(dongle() == (0, ""), "setting off, no adapter: none")

    # The X6200: never, whatever the setting
    s = fresh()
    s.own()
    s.dongle()
    setting(True)
    write(os.path.join(BUILD, "compatible"), "xiegu,x6200\0allwinner,sun50i-h616\0")
    check(dongle() == (0, "hci0") and dongle("dongle")[0] != 0, "X6200, setting on, a dongle: hci0, no dongle")
    check(dongle("apply") == (0, "") and read(unbind) == "", "X6200: apply lets nothing go")

    check(dongle("odd")[0] != 0, "an unknown command fails")


# btaudio-ctl sync

CARD_HEADSET = """Card #3
	Name: bluez_card.11_22_33_44_55_66
	Driver: module-bluez5-device.c
	Profiles:
		a2dp_sink: High Fidelity Playback (A2DP Sink) (sinks: 1, sources: 0, priority: 40, available: yes)
		handsfree_head_unit: Handsfree Head Unit (HFP) (sinks: 1, sources: 1, priority: 30, available: yes)
		off: Off (sinks: 0, sources: 0, priority: 0, available: yes)
	Active Profile: %s
	Ports:
		headset-output: Headset (type: Headset, priority: 0, latency offset: 0 usec, available)
			Part of profile(s): a2dp_sink, handsfree_head_unit
"""
CARD_PHONES = """Card #4
	Name: bluez_card.AA_BB_CC_DD_EE_FF
	Driver: module-bluez5-device.c
	Profiles:
		a2dp_sink: High Fidelity Playback (A2DP Sink) (sinks: 1, sources: 0, priority: 40, available: yes)
		off: Off (sinks: 0, sources: 0, priority: 0, available: yes)
	Active Profile: a2dp_sink
"""
CARD_PHONE = """Card #5
	Name: bluez_card.12_34_56_78_9A_BC
	Driver: module-bluez5-device.c
	Profiles:
		a2dp_source: High Fidelity Capture (A2DP Source) (sinks: 0, sources: 1, priority: 20, available: yes)
		handsfree_audio_gateway: Handsfree Gateway (HFP) (sinks: 1, sources: 1, priority: 30, available: yes)
		off: Off (sinks: 0, sources: 0, priority: 0, available: yes)
	Active Profile: off
"""
MOD_OFONO = "11\tmodule-bluetooth-discover\theadset=ofono\n"
MOD_NATIVE = "11\tmodule-bluetooth-discover\theadset=native enable_native_hsp_hs=false\n"


def audio(in_use, modules, cards, sinks="", sources="", pulse=True):
    for f, t in (("modules", modules), ("cards", cards), ("sinks", sinks), ("sources", sources)):
        write(os.path.join(BUILD, f), t)
    nop = os.path.join(BUILD, "no-pulse")
    if pulse and os.path.exists(nop):
        os.unlink(nop)
    elif not pulse:
        write(nop, "")
    log = os.path.join(BUILD, "calls.log")
    if os.path.exists(log):
        os.unlink(log)
    run = os.path.join(BUILD, "run")
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    r = subprocess.run(["sh", BTAUDIO_CTL, "sync"], capture_output=True, text=True, timeout=20,
                       env=env({"BTAUDIO_CONF": os.path.join(BUILD, "btaudio.conf"), "BTAUDIO_RUN": run,
                                "BTAUDIO_DONGLE": os.path.join(BIN, "dongle-yes" if in_use else "dongle-no")}))
    calls = [l for l in (read(log) or "").splitlines() if not l.startswith("pactl list")]
    return r.returncode, calls, read(os.path.join(run, "btaudio-mic")), read(os.path.join(run, "btaudio-hp"))


def test_btaudio():
    hs = "bluez_sink.11_22_33_44_55_66.handsfree_head_unit"
    a2 = "bluez_sink.11_22_33_44_55_66.a2dp_sink"
    mic = "bluez_source.11_22_33_44_55_66.handsfree_head_unit"
    src_line = "7\t%s\tmodule-bluez5-device.c\ts16le 1ch 16000Hz\tRUNNING\n" % mic

    # No dongle in use, as every radio: nothing loaded again, a headset kept on (or put back to) A2DP, no mic
    rc, calls, m, hp = audio(False, MOD_OFONO, CARD_HEADSET % "a2dp_sink" + CARD_PHONES + CARD_PHONE,
                             sinks="9\t%s\tmodule-bluez5-device.c\n" % a2)
    check(rc == 0 and not any("bluetooth-discover" in c for c in calls), "no dongle: the backend left as it is")
    check(not any("set-card-profile" in c for c in calls), "no dongle, all on A2DP: no profile change: %r" % calls)
    check(m is None and hp == a2 + "\n", "no dongle: no mic, the A2DP headphones in the state")
    check(any("load-module module-loopback" in c and "sink=%s " % a2 in c for c in calls), "no dongle: RX loopback")

    rc, calls, m, hp = audio(False, MOD_OFONO, CARD_HEADSET % "handsfree_head_unit")
    check("pactl set-card-profile bluez_card.11_22_33_44_55_66 a2dp_sink" in calls,
          "no dongle, a headset on HFP: back on A2DP")

    # The radio's own back with the native backend still loaded (setting turned off): ofono again
    rc, calls, m, hp = audio(False, MOD_NATIVE, "")
    check(calls[:2] == ["pactl unload-module module-bluetooth-discover",
                        "pactl load-module module-bluetooth-discover headset=ofono"],
          "no dongle, native loaded: back to no headset backend: %r" % calls[:2])

    # A dongle in use: the native backend, a headset with a mic on HFP, headphones and a phone left alone
    rc, calls, m, hp = audio(True, MOD_OFONO, CARD_HEADSET % "a2dp_sink" + CARD_PHONES + CARD_PHONE)
    check(calls[:2] == ["pactl unload-module module-bluetooth-discover",
                        "pactl load-module module-bluetooth-discover headset=native enable_native_hsp_hs=false"],
          "dongle: the native backend, HSP off: %r" % calls[:2])
    prof = [c for c in calls if "set-card-profile" in c]
    check(prof == ["pactl set-card-profile bluez_card.11_22_33_44_55_66 handsfree_head_unit"],
          "dongle: the headset on HFP, the headphones on A2DP, the phone left alone: %r" % prof)

    # On HFP: its mic named for the GUI, RX to its sink, the latency offset on its port
    rc, calls, m, hp = audio(True, MOD_NATIVE, CARD_HEADSET % "handsfree_head_unit",
                             sinks="9\t%s\tmodule-bluez5-device.c\n" % hs, sources=src_line)
    check(not any("bluetooth-discover" in c or "set-card-profile" in c for c in calls),
          "dongle, all in place: nothing loaded or switched again")
    check(m == mic + "\n", "dongle: the mic for the side PTT key: %r" % m)
    check(hp == hs + "\n", "dongle: the hands-free sink as headphones")
    check("pactl set-port-latency-offset bluez_card.11_22_33_44_55_66 headset-output 0" in calls,
          "dongle: the card's name without .handsfree_head_unit")
    check(any("load-module module-loopback" in c and "sink=%s " % hs in c for c in calls), "dongle: RX loopback")

    # The headset gone: the mic with it
    rc, calls, m, hp = audio(True, MOD_NATIVE, "")
    check(m is None and hp is None, "the headset gone: no mic, no headphones")

    # PulseAudio not up yet: no backend loaded blind
    rc, calls, m, hp = audio(True, "", "", pulse=False)
    check(not any("load-module module-bluetooth-discover" in c for c in calls), "no PulseAudio: nothing loaded")


def main():
    shutil.rmtree(BUILD, ignore_errors=True)
    os.makedirs(BIN)
    stubs()
    test_bt_dongle()
    test_btaudio()
    print("bt_dongle scripts: %d checks, %d failed" % (checks, failed))
    if not failed:
        shutil.rmtree(BUILD, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
