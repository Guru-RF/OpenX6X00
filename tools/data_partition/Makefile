# Host test of the DATA partition script (rootfs/etc/init.d/S00adata, #47): `make` runs it, run.sh with the cases in
# cases.sh. Each case is a fake card: a sparse file with the card image's partition table (X6100 and X6200 alike),
# the script run on it through the shims in bin/ (the real parted, mkfs.vfat and blkid on the card file, mount as a
# link to a folder per filesystem), with the radio's own x6100-starttrace. A fresh card; a new image over a sound
# DATA, a damaged one (sector 0, 4 or 16 KiB, 64 KiB, 4 MiB lost; a 4 and a 64 GB card), exFAT, ext4, NTFS, a PC's FAT32
# or FAT16; a card v0.3.8 left without DATA; DATA damaged after 40 v0.3.8 starts without it, or after its first
# run; DATA formatted on a PC (empty, Windows' folder, files of its own, a stray -wal, a QSO log or journal of its
# own beside the system's hot -journal); copies that fail or are cut short, a v0.3.8 found_on_system of older
# files; starts without DATA in turn. v0.3.8's script (git show v0.3.8:...) makes the cards it left behind and
# gives the normal start's tool calls to compare.
# Needs bash, sfdisk, parted, mkfs.vfat, mkfs.ext4, blkid (util-linux), sqlite3, python3; mkfs.exfat for that case.
# The cards are sparse (some 300 MB written, in build/runs), and go at the end or when the run is stopped (KEEP=1
# keeps them; after a kill -9, `make clean`). ONLY="fresh reimage" runs those cases.
# TARGET=<a buildroot target>: busybox's applets and blkid from it (qemu-arm-static), IMGSH=1 its bash as well.

all: test

test:
	bash run.sh

clean:
	rm -rf build

.PHONY: all test clean
