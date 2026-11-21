# rtw88 patches (the X6200's RTL8723DU WiFi)

Applied by `buildroot/local-build.sh` to lwfinger/rtw88 at the buildroot tree's pin (461b696).

- `0001`: upstream's TX report timeout fixes.
- `0002`: a notch for the X6200's own spurs in WiFi channel 11, set by
  `rootfs/etc/modprobe.d/x6200-wifi-spur.conf`. Explained below.

## Why channel 11 failed on the X6200

**Symptom.** On an access point on channel 11 the X6200's WiFi stalled for a second or more at a time and dropped
connections under steady upload. An X6100 beside it, on the same access point, did not. So the cause was the
X6200, not the access point or the room.

**Cause: the radio jams itself.** A HackRF beside the X6200 (receive only, 30 s medians across 2.40-2.50 GHz), radio
on against radio off:

| Line | With the X6200 on | With it off |
|---|---|---|
| 2457.60 MHz | +7.3 dB over the band (50 kHz bins), +11.9 dB (10 kHz bins) | none |
| 2470.61 MHz | +10.6 dB | none |
| the rest of 2.4 GHz | | the same to 0.2 dB |

2457.6 MHz is 10 x 245.76 MHz (also 40 x 61.44 and 200 x 12.288 MHz), the X6200's own clock family. The 2470.6 MHz
line's source is not known. Channel 11 runs 2452-2472 MHz, so both fall inside it, 4.4 MHz below and 8.6 MHz above
its centre. The WiFi chip sits inside the same radio, centimetres from their source.

**Why a notch is the fix.** The RTL8723D's receiver already has the tools, and this driver already uses them for the
chip's *own* spur at 2480 MHz on channels 13 and 14 (`rtw8723d_cfg_notch`):

- the NBI notch (register 0xc40), which removes a narrow band before demodulation;
- the CSI mask (0xd40-0xd4c, enabled by 0xd2c bit 28), which leaves the OFDM tones under a spur out of the channel
  estimate, so one jammed tone no longer corrupts the whole frame.

`0002` adds `host_spur_khz`, so a board can name its own spurs and get the same treatment on whatever channel they
fall in. The register values follow the driver's own coded cases: the NBI index from phydm's 20 MHz table (giving
the coded 0xb for +8.0 MHz and 0x5 for -4.0 MHz), and CSI bit t for tone +t, 128 - t for tone -t. On channel 11 that
gives NBI index 6, CSI bit 114 for 2457.6 MHz and bits 27-28 for 2470.6 MHz.

Avoiding channel 11 is not a fix. The radio does not choose the access point's channel, and steering away from a
strong channel-11 access point to a weak one elsewhere is worse: tried, it put the X6200 at -76 to -81 dBm on
channel 6 and about 1 Mbit/s.

## Proof

WSJT-X's TCI audio from the radio (48 kHz float32 stereo, 3 Mbit/s upload), 3 minutes per run, channel 11, the
X6200 joined at -50 to -69 dBm:

| Notch | Real time held |
|---|---|
| off (set by hand in debugfs) | 33 % mean |
| on (by hand) | 99.9 %, 100 % |
| off (`host_spur_khz` unset, fresh driver load) | connection dropped at 74 s |
| on (`host_spur_khz` set, fresh driver load) | 99.4 %, 99.7 % |

## Checking a radio

```sh
cat /sys/module/rtw_8723d/parameters/host_spur_khz   # 2457600,2470614 with the option and this driver
iw dev wlan0 link                                    # freq: 2462 is channel 11
```

On channel 11 with the notch, rtw88's debugfs `read_reg` gives 0xc40 bits 28:24 = 6 with bit 9 set (0x0678423f on
ours; 0x1f78403f without), 0xd2c bit 28 set, `0xd40 = 0x18000000` and `0xd4c = 0x00040000`.

## Limits

- Measured on one X6200. The lines come from the board's clocks, so every X6200 should have them within the
  crystals' tolerance (about 0.1 MHz at 2.4 GHz), under one 312.5 kHz tone.
- Channels 9, 10 and 12 also hold one or both lines and get the notch, but only channel 11 was measured.
- Channel 13 is not covered. There the driver sets the chip's own 2480 MHz notch, and 2470.6 MHz sits 1.4 MHz below
  that channel's centre.
- The driver is in the card image: an update bundle brings the modprobe option but not the driver. A driver without
  the parameter logs "unknown parameter" and loads as before.
