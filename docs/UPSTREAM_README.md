# Notes from the upstream x6100_gui README

The upstream project is the LVGL GUI for the Xiegu X6100 by R1CBU, carried on
by Georgy Dyuldin (gdyuldin/x6100_gui). These are the parts of its README that
still apply here, in our words, with the build steps written for this
repository.

## Putting the image on a card

Each release has an `sdcard.img` in its Assets. Write it to a microSD card
with balenaEtcher or Rufus, put the card in the radio and power it on.

## Bringing in an ADI log

Callsigns already worked are marked in the UI. To bring in the QSOs of an
older log, copy the ADI file to the `DATA` partition of the card as
`incoming_log.adi`. At the next start its records go into the radio's own log
and the file is renamed `incoming_log.adi.bak`. A record that names neither
OPERATOR nor STATION_CALLSIGN is logged under the radio's callsign (Settings >
General > STATION).

The `DATA` partition only exists after the radio has started once with the
card in.

## Taking the FT8/FT4 log out

FT8/FT4 QSOs are written to `ft_log.adi` on the `DATA` partition, ready to
upload to an online log.

## Building on a PC

Both trees side by side in one folder, with their submodules:

    mkdir x6100 && cd x6100
    git clone --recurse-submodules https://github.com/gdyuldin/AetherX6100Buildroot
    git clone --recurse-submodules https://github.com/Guru-RF/OpenX6X00

The buildroot tree first (long, the first time):

    cd AetherX6100Buildroot && ./br_config.sh
    cd build && make && cd ../..

Then the GUI, with the compiler and libraries of that tree
(`buildroot/README.md`):

    cd OpenX6X00/buildroot && ./build.sh
