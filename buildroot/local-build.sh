#!/bin/bash
#
# Builds the SD card image locally, as .github/workflows/main.yml does on GitHub:
# AetherX6100Buildroot (or, with MODEL=x6200, AetherX6200Buildroot) next to this repository,
# patched to build this GUI and web server from the checkout, then buildroot. Run it in the
# Ubuntu 22.04 container of the Dockerfile here, as yourself, with the parent of both
# repositories mounted on /work:
#
#   docker build -t openx6x00-build buildroot
#   docker run --rm -u $(id -u):$(id -g) -e HOME=/work/.brhome -v $(dirname $PWD):/work \
#       openx6x00-build bash /work/OpenX6X00/buildroot/local-build.sh
#   docker run --rm -u $(id -u):$(id -g) -e HOME=/work/.brhome -e MODEL=x6200 -v $(dirname $PWD):/work \
#       openx6x00-build bash /work/OpenX6X00/buildroot/local-build.sh
#
# The image lands in ../AetherX6100Buildroot/build/images/OpenX6X00-local-sdcard.img, the
# X6200's in ../AetherX6200Buildroot/build/images/OpenX6X00-x6200-local-sdcard.img.
# About an hour the first time; ccache in $HOME/.buildroot-ccache keeps the next ones short.
set -eo pipefail

# The two radios share this GUI (src/hw/hw.h picks the backend, cmake -DHW_BACKEND) and the
# web server, and each has its own buildroot tree from Aether Radio: the X6200's names its
# packages x6200-gui and aether-x6200-control, its board X6200
MODEL=${MODEL:-x6100}
case $MODEL in
    x6100) M=X6100; BRDEF=AetherX6100Buildroot; GUI_PKG=x6100-gui; GUI_VAR=X6100_GUI; OUT=OpenX6X00 ;;
    x6200) M=X6200; BRDEF=AetherX6200Buildroot; GUI_PKG=x6200-gui; GUI_VAR=X6200_GUI; OUT=OpenX6X00-x6200 ;;
    *) echo "MODEL must be x6100 or x6200, not '$MODEL'"; exit 1 ;;
esac
GUI=${GUI_DIR:-/work/OpenX6X00}
BR=${BR_DIR:-/work/$BRDEF}
EXT=$BR/br2_external
GUI_MK=$EXT/package/$GUI_PKG/${GUI_PKG/-/_}.mk
GUI_CONFIG_IN=$EXT/package/$GUI_PKG/Config.in
WEB_MK=$EXT/package/x6100-webserver/x6100_webserver.mk
DEFCONFIG=$EXT/configs/${M}_defconfig
BOARD=$EXT/board/$M/linux
KERNEL_DEFCONFIG=$BOARD/sun8i-r16-$MODEL\_defconfig
mkdir -p "$HOME"

echo "== $M: GUI package $GUI_PKG from $GUI"
sed -i "s,^${GUI_VAR}_SITE = .*,${GUI_VAR}_SITE = $GUI,g" $GUI_MK
sed -i "s,^${GUI_VAR}_SITE_METHOD = git,${GUI_VAR}_SITE_METHOD = local,g" $GUI_MK
sed -i "s,^${GUI_VAR}_GIT_SUBMODULES,# ${GUI_VAR}_GIT_SUBMODULES,g" $GUI_MK
sed -i "s,^X6100_WEBSERVER_SITE = .*,X6100_WEBSERVER_SITE = $GUI/webserver,g" $WEB_MK
sed -i "s,^X6100_WEBSERVER_SITE_METHOD = git,X6100_WEBSERVER_SITE_METHOD = local,g" $WEB_MK
grep -E "^(${GUI_VAR}|X6100_WEBSERVER)_SITE" $GUI_MK $WEB_MK
# The backend: the GUI's CMake defaults to the X6100. Before the eval line: what a .mk sets after
# $(eval $(cmake-package)) the package never sees
sed -i "/_CONF_OPTS += -DHW_BACKEND=/d" $GUI_MK
sed -i "s|^\$(eval \$(cmake-package))|${GUI_VAR}_CONF_OPTS += -DHW_BACKEND=$MODEL\n\n&|" $GUI_MK
grep -q "^${GUI_VAR}_CONF_OPTS += -DHW_BACKEND=$MODEL" $GUI_MK

echo "== JS8 dependencies, root certificates, libdrm"
grep -q "boost fftw-single" $GUI_MK || sed -i "s,^${GUI_VAR}_DEPENDENCIES = \(.*\)$,${GUI_VAR}_DEPENDENCIES = \1 boost fftw-single," $GUI_MK
grep -q "^BR2_PACKAGE_BOOST=y" $DEFCONFIG || echo "BR2_PACKAGE_BOOST=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_LIBDRM=y" $DEFCONFIG || echo "BR2_PACKAGE_LIBDRM=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_CA_CERTIFICATES=y" $DEFCONFIG || echo "BR2_PACKAGE_CA_CERTIFICATES=y" >> $DEFCONFIG
# Python's ssl module and the openssl command: HTTPS from the helpers (spots, QRZ, GitHub) and the
# web server's certificate; the upstream defconfig left both out, HTTPS failed silently
grep -q "^BR2_PACKAGE_PYTHON3_SSL=y" $DEFCONFIG || echo "BR2_PACKAGE_PYTHON3_SSL=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_LIBOPENSSL_BIN=y" $DEFCONFIG || echo "BR2_PACKAGE_LIBOPENSSL_BIN=y" >> $DEFCONFIG
# mDNS: x6100.local (rootfs/etc/avahi, S39hostname). The GUI package selects Avahi so that it is
# installed first and our avahi-daemon.conf is the one left in the image
grep -q "^BR2_PACKAGE_AVAHI=y" $DEFCONFIG || echo "BR2_PACKAGE_AVAHI=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_AVAHI_DAEMON=y" $DEFCONFIG || echo "BR2_PACKAGE_AVAHI_DAEMON=y" >> $DEFCONFIG
grep -q "select BR2_PACKAGE_AVAHI_DAEMON" $GUI_CONFIG_IN || sed -i "s/^\tselect BR2_PACKAGE_LIBPNG$/\tselect BR2_PACKAGE_LIBPNG\n\tselect BR2_PACKAGE_AVAHI\n\tselect BR2_PACKAGE_AVAHI_DAEMON/" $GUI_CONFIG_IN
grep -q "avahi" $GUI_MK || sed -i "/^${GUI_VAR}_DEPENDENCIES/s/$/ avahi/" $GUI_MK
grep -q "libdrm" $GUI_MK || sed -i "/^${GUI_VAR}_DEPENDENCIES/s/$/ libdrm/" $GUI_MK

echo "== No RHVoice: PulseAudio selected by the GUI itself"
sed -i "/^${GUI_VAR}_DEPENDENCIES/s/\brhvoice\b/pulseaudio/" $GUI_MK
sed -i "s/select BR2_PACKAGE_RHVOICE$/select BR2_PACKAGE_PULSEAUDIO/" $GUI_CONFIG_IN
sed -i "/^BR2_PACKAGE_RHVOICE/d" $DEFCONFIG
grep "^${GUI_VAR}_DEPENDENCIES" $GUI_MK
! grep -in "rhvoice" $GUI_MK $GUI_CONFIG_IN $DEFCONFIG

echo "== System cleanup: no PCI firmware, PulseAudio at 44.1 kHz, one Bluetooth setup"
for o in BROADCOM_TIGON3 BNX2X CXGB4_T4 CXGB4_T5 INTEL_E100 INTEL_ICE MICROCHIP_VSC85XX_PHY QLOGIC_4X RTL_8169; do
    sed -i "/^BR2_PACKAGE_LINUX_FIRMWARE_${o}=y/d" $DEFCONFIG
done
# The X6200 tree ships no daemon.conf: ours below (buildroot/br2_external) is at 44.1 kHz already
[ -f $BOARD/rootfs-overlay/etc/pulse/daemon.conf ] && sed -i "s/^default-sample-rate = 48000/default-sample-rate = 44100/" $BOARD/rootfs-overlay/etc/pulse/daemon.conf
mkdir -p $BOARD/rootfs-overlay/usr/bin
cp $GUI/buildroot/bt_start.sh $BOARD/rootfs-overlay/usr/bin/bt_start.sh

echo "== OpenX6X00 additions to the buildroot tree (buildroot/br2_external in this repository)"
# The rtl8723bu WiFi driver package (the X6100's USB WiFi; the X6200's is on the board), the DATA
# partition repair at boot, the USB tethering profile, the Bluetooth, NTP and PulseAudio settings:
# copied over the upstream tree. Kept under board/X6100 in this repository; the X6200 tree gets
# the same files under its board
cp -a $GUI/buildroot/br2_external/package $EXT/
cp -a $GUI/buildroot/br2_external/board/X6100/linux/. $BOARD/
if [ $MODEL = x6200 ]; then
    rm -f $BOARD/rootfs-overlay/etc/modprobe.d/x6100-wifi.conf
    sed -i "s/BR2_EXTERNAL_X6100_PATH/BR2_EXTERNAL_X6200_PATH/" $BOARD/device_table.txt 2>/dev/null || true
fi
grep -q "^default-sample-rate = 44100" $BOARD/rootfs-overlay/etc/pulse/daemon.conf
rm -f $BOARD/rootfs-overlay/etc/init.d/S01create_data   # S00adata does it, with the repair
# S00adata and the NTP hook are in rootfs/ (installed by the GUI's CMake, so the update bundle carries
# them); the overlay's copies would be laid over them last
rm -f $BOARD/rootfs-overlay/etc/init.d/S00adata $BOARD/rootfs-overlay/etc/NetworkManager/dispatcher.d/99-ntp-sync.sh
rm -f $BR/build/target/etc/init.d/S01create_data                         # and from an earlier build's target
# The legacy updater (any x6100_r1cbu_*.tgz on the DATA partition unpacked over / at boot, unsigned,
# unchecked): OpenX6X00 is installed as a card image, so it goes
rm -f $BOARD/rootfs-overlay/etc/init.d/S02update $BR/build/target/etc/init.d/S02update
if [ $MODEL = x6100 ]; then
    grep -q "rtl8723bu-wifi/Config.in" $EXT/Config.in || echo 'source "$BR2_EXTERNAL_X6100_PATH/package/rtl8723bu-wifi/Config.in"' >> $EXT/Config.in
    grep -q "^BR2_PACKAGE_RTL8723BU_WIFI=y" $DEFCONFIG || echo "BR2_PACKAGE_RTL8723BU_WIFI=y" >> $DEFCONFIG
fi
grep -q "^BR2_ROOTFS_DEVICE_TABLE=" $DEFCONFIG || echo "BR2_ROOTFS_DEVICE_TABLE=\"system/device_table.txt \$(BR2_EXTERNAL_${M}_PATH)/board/$M/linux/device_table.txt\"" >> $DEFCONFIG
for o in CONFIG_USB_NET_RNDIS_HOST=m CONFIG_IP_ADVANCED_ROUTER=y CONFIG_IP_MULTIPLE_TABLES=y; do
    grep -q "^$o" $KERNEL_DEFCONFIG || echo "$o" >> $KERNEL_DEFCONFIG
done

echo "== Kernel: UHID for Bluetooth Low Energy keyboards"
grep -q "^CONFIG_UHID=" $KERNEL_DEFCONFIG || echo "CONFIG_UHID=m" >> $KERNEL_DEFCONFIG

echo "== Boot logo background"
cp $GUI/boot/kernel_logo.png $BOARD/logo.png

echo "== Configure"
cd $BR && bash ./br_config.sh

echo "== Build"
cd $BR/build
make lame mpg123
make libsndfile-dirclean
# The GUI and the web server come from this repository (a local site): buildroot syncs them
# once and never again on its own, so they are re-synced and rebuilt every run
# The sync into build/<gui package>-*/ adds and updates but never deletes: what left the repository
# would stay there and be installed again (an old S01create_data was), as would what an earlier
# build put in target/. A directory that held another GUI's sources (the stock X6200 GUI, in a tree
# built stock before) is wiped once: its files would be compiled along with ours
if ls $BR/build/build/$GUI_PKG-*/.stamp_built > /dev/null 2>&1; then
    for d in $BR/build/build/$GUI_PKG-*/; do
        [ -d "$d" ] && [ ! -f "$d/.openx6x00" ] && { echo "   wiping $d (not synced from OpenX6X00 before)"; rm -rf "$d"; }
    done
    # The synced rootfs copy goes first, and target/ loses what rootfs/ no longer has
    for d in $BR/build/build/$GUI_PKG-*/rootfs; do
        [ -d "$d" ] || continue
        (cd "$d" && find . -type f) | while read -r f; do
            [ -e "$GUI/rootfs/$f" ] || rm -f "$d/$f" "$BR/build/target/$f"
        done
    done
    find $BR/build/target/usr/sbin -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
    # The GUI is configured again, not only rebuilt: its options (HW_BACKEND above) live in CMake's
    # cache, which a rebuild keeps
    make $GUI_PKG-reconfigure x6100-webserver-rebuild
else
    # A tree built for the first time (the GitHub runner): the whole image in buildroot's own
    # order. The GUI package names only part of what it links, the upstream control library
    # nothing at all (libgpiod): asked for on their own, as above, they came before libgpiod
    # and the rest were there and failed
    echo "   first build of this tree: everything, in buildroot's order"
    make
fi
for d in $BR/build/build/$GUI_PKG-*/; do touch "$d/.openx6x00"; done
# Packages whose options changed after an earlier build here are not rebuilt by buildroot on
# its own: python3 (the ssl module) and openssl (the command) are reconfigured when missing
ls $BR/build/target/usr/lib/python3.11/lib-dynload/_ssl*.so > /dev/null 2>&1 || make python3-reconfigure
[ -x $BR/build/target/usr/bin/openssl ] || make libopenssl-reconfigure
make

echo "== Update bundle (tools/update/mkbundle.py): what x6100-update installs on a running radio"
# Signed with $UPDATE_KEY (default: $HOME/.config/openx6x00/update-key.pem, so inside the container
# /work/.brhome/.config/openx6x00/update-key.pem); unsigned without it, which a radio only takes with
# /mnt/update/allow-unsigned present
VER=$(sed -n 's/.*define VERSION.* v\([0-9.]*\).*/\1/p' $GUI/src/main.h)
KEY=${UPDATE_KEY:-$HOME/.config/openx6x00/update-key.pem}
KEYARG=""; [ -f "$KEY" ] && KEYARG="--key $KEY"
python3 $GUI/tools/update/mkbundle.py --target $BR/build/target --manifest $(ls -d $BR/build/build/$GUI_PKG-*/install_manifest.txt | head -1) \
    --version $VER --model $MODEL --out $BR/build/images/$OUT-v$VER-update.tar.gz $KEYARG

if [ $MODEL = x6100 ]; then
    echo "== Our U-Boot over buildroot's, 8 KB into the card"
    dd if=$GUI/u-boot/u-boot-sunxi-with-spl.bin of=$BR/build/images/sdcard.img seek=8K bs=1 conv=notrunc
fi
cp $BR/build/images/sdcard.img $BR/build/images/$OUT-local-sdcard.img
ls -la $BR/build/images/$OUT-local-sdcard.img
echo "== done"
