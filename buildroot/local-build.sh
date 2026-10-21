#!/bin/bash
#
# Builds the SD card image locally, as .github/workflows/main.yml does on GitHub:
# AetherX6100Buildroot next to this repository, patched to build this GUI and web server
# from the checkout, then buildroot. Run it in the Ubuntu 22.04 container of the Dockerfile
# here, as yourself, with the parent of both repositories mounted on /work:
#
#   docker build -t openx6x00-build buildroot
#   docker run --rm -u $(id -u):$(id -g) -e HOME=/work/.brhome -v $(dirname $PWD):/work \
#       openx6x00-build bash /work/OpenX6X00/buildroot/local-build.sh
#
# The image lands in ../AetherX6100Buildroot/build/images/OpenX6X00-local-sdcard.img.
# About an hour the first time; ccache in $HOME/.buildroot-ccache keeps the next ones short.
set -eo pipefail

GUI=${GUI_DIR:-/work/OpenX6X00}
BR=${BR_DIR:-/work/AetherX6100Buildroot}
EXT=$BR/br2_external
GUI_MK=$EXT/package/x6100-gui/x6100_gui.mk
WEB_MK=$EXT/package/x6100-webserver/x6100_webserver.mk
DEFCONFIG=$EXT/configs/X6100_defconfig
mkdir -p "$HOME"

echo "== GUI package from $GUI"
sed -i "s,^X6100_GUI_SITE = .*,X6100_GUI_SITE = $GUI,g" $GUI_MK
sed -i "s,^X6100_GUI_SITE_METHOD = git,X6100_GUI_SITE_METHOD = local,g" $GUI_MK
sed -i "s,^X6100_GUI_GIT_SUBMODULES,# X6100_GUI_GIT_SUBMODULES,g" $GUI_MK
sed -i "s,^X6100_WEBSERVER_SITE = .*,X6100_WEBSERVER_SITE = $GUI/webserver,g" $WEB_MK
sed -i "s,^X6100_WEBSERVER_SITE_METHOD = git,X6100_WEBSERVER_SITE_METHOD = local,g" $WEB_MK
grep -E "^X6100_(GUI|WEBSERVER)_SITE" $GUI_MK $WEB_MK

echo "== JS8 dependencies, root certificates, libdrm"
grep -q "boost fftw-single" $GUI_MK || sed -i "s,^X6100_GUI_DEPENDENCIES = \(.*\)$,X6100_GUI_DEPENDENCIES = \1 boost fftw-single," $GUI_MK
grep -q "^BR2_PACKAGE_BOOST=y" $DEFCONFIG || echo "BR2_PACKAGE_BOOST=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_CA_CERTIFICATES=y" $DEFCONFIG || echo "BR2_PACKAGE_CA_CERTIFICATES=y" >> $DEFCONFIG
# Python's ssl module and the openssl command: HTTPS from the helpers (spots, QRZ, GitHub) and the
# web server's certificate; the upstream defconfig left both out, HTTPS failed silently
grep -q "^BR2_PACKAGE_PYTHON3_SSL=y" $DEFCONFIG || echo "BR2_PACKAGE_PYTHON3_SSL=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_LIBOPENSSL_BIN=y" $DEFCONFIG || echo "BR2_PACKAGE_LIBOPENSSL_BIN=y" >> $DEFCONFIG
# mDNS: x6100.local (rootfs/etc/avahi, S39hostname). The GUI package selects Avahi so that it is
# installed first and our avahi-daemon.conf is the one left in the image
grep -q "^BR2_PACKAGE_AVAHI=y" $DEFCONFIG || echo "BR2_PACKAGE_AVAHI=y" >> $DEFCONFIG
grep -q "^BR2_PACKAGE_AVAHI_DAEMON=y" $DEFCONFIG || echo "BR2_PACKAGE_AVAHI_DAEMON=y" >> $DEFCONFIG
grep -q "select BR2_PACKAGE_AVAHI_DAEMON" $EXT/package/x6100-gui/Config.in || sed -i "s/^\tselect BR2_PACKAGE_LIBPNG$/\tselect BR2_PACKAGE_LIBPNG\n\tselect BR2_PACKAGE_AVAHI\n\tselect BR2_PACKAGE_AVAHI_DAEMON/" $EXT/package/x6100-gui/Config.in
grep -q "avahi" $GUI_MK || sed -i "/^X6100_GUI_DEPENDENCIES/s/$/ avahi/" $GUI_MK
grep -q "libdrm" $GUI_MK || sed -i "/^X6100_GUI_DEPENDENCIES/s/$/ libdrm/" $GUI_MK

echo "== No RHVoice: PulseAudio selected by the GUI itself"
sed -i "/^X6100_GUI_DEPENDENCIES/s/\brhvoice\b/pulseaudio/" $GUI_MK
sed -i "s/select BR2_PACKAGE_RHVOICE$/select BR2_PACKAGE_PULSEAUDIO/" $EXT/package/x6100-gui/Config.in
sed -i "/^BR2_PACKAGE_RHVOICE/d" $DEFCONFIG
grep "^X6100_GUI_DEPENDENCIES" $GUI_MK
! grep -in "rhvoice" $GUI_MK $EXT/package/x6100-gui/Config.in $DEFCONFIG

echo "== System cleanup: no PCI firmware, PulseAudio at 44.1 kHz, one Bluetooth setup"
for o in BROADCOM_TIGON3 BNX2X CXGB4_T4 CXGB4_T5 INTEL_E100 INTEL_ICE MICROCHIP_VSC85XX_PHY QLOGIC_4X RTL_8169; do
    sed -i "/^BR2_PACKAGE_LINUX_FIRMWARE_${o}=y/d" $DEFCONFIG
done
sed -i "s/^default-sample-rate = 48000/default-sample-rate = 44100/" $EXT/board/X6100/linux/rootfs-overlay/etc/pulse/daemon.conf
cp $GUI/buildroot/bt_start.sh $EXT/board/X6100/linux/rootfs-overlay/usr/bin/bt_start.sh
grep -q "^default-sample-rate = 44100" $EXT/board/X6100/linux/rootfs-overlay/etc/pulse/daemon.conf

echo "== OpenX6X00 additions to the buildroot tree (buildroot/br2_external in this repository)"
# The rtl8723bu WiFi driver package, the DATA partition repair at boot, the USB tethering
# profile, the Bluetooth, NTP and PulseAudio settings: copied over the upstream tree
cp -a $GUI/buildroot/br2_external/. $EXT/
rm -f $EXT/board/X6100/linux/rootfs-overlay/etc/init.d/S01create_data   # S00adata does it, with the repair
# S00adata and the NTP hook are in rootfs/ (installed by the GUI's CMake, so the update bundle carries
# them); the overlay's copies would be laid over them last
rm -f $EXT/board/X6100/linux/rootfs-overlay/etc/init.d/S00adata $EXT/board/X6100/linux/rootfs-overlay/etc/NetworkManager/dispatcher.d/99-ntp-sync.sh
rm -f $BR/build/target/etc/init.d/S01create_data                         # and from an earlier build's target
# The legacy updater (any x6100_r1cbu_*.tgz on the DATA partition unpacked over / at boot, unsigned,
# unchecked): OpenX6X00 is installed as a card image, so it goes
rm -f $EXT/board/X6100/linux/rootfs-overlay/etc/init.d/S02update $BR/build/target/etc/init.d/S02update
grep -q "rtl8723bu-wifi/Config.in" $EXT/Config.in || echo 'source "$BR2_EXTERNAL_X6100_PATH/package/rtl8723bu-wifi/Config.in"' >> $EXT/Config.in
grep -q "^BR2_PACKAGE_RTL8723BU_WIFI=y" $DEFCONFIG || echo "BR2_PACKAGE_RTL8723BU_WIFI=y" >> $DEFCONFIG
grep -q "^BR2_ROOTFS_DEVICE_TABLE=" $DEFCONFIG || echo 'BR2_ROOTFS_DEVICE_TABLE="system/device_table.txt $(BR2_EXTERNAL_X6100_PATH)/board/X6100/linux/device_table.txt"' >> $DEFCONFIG
D=$EXT/board/X6100/linux/sun8i-r16-x6100_defconfig
for o in CONFIG_USB_NET_RNDIS_HOST=m CONFIG_IP_ADVANCED_ROUTER=y CONFIG_IP_MULTIPLE_TABLES=y; do
    grep -q "^$o" $D || echo "$o" >> $D
done

echo "== Kernel: UHID for Bluetooth Low Energy keyboards"
D=$EXT/board/X6100/linux/sun8i-r16-x6100_defconfig
grep -q "^CONFIG_UHID=" $D || echo "CONFIG_UHID=m" >> $D

echo "== Boot logo background"
cp $GUI/boot/kernel_logo.png $EXT/board/X6100/linux/logo.png

echo "== Configure"
cd $BR && bash ./br_config.sh

echo "== Build"
cd $BR/build
make lame mpg123
make libsndfile-dirclean
# The GUI and the web server come from this repository (a local site): buildroot syncs them
# once and never again on its own, so they are re-synced and rebuilt every run
# The sync into build/x6100-gui-*/ adds and updates but never deletes: what left the repository
# would stay there and be installed again (an old S01create_data was), as would what an earlier
# build put in target/. The synced rootfs copy goes first, and target/ loses what rootfs/ no longer has
for d in $BR/build/build/x6100-gui-*/rootfs; do
    [ -d "$d" ] || continue
    (cd "$d" && find . -type f) | while read -r f; do
        [ -e "$GUI/rootfs/$f" ] || rm -f "$d/$f" "$BR/build/target/$f"
    done
done
find $BR/build/target/usr/sbin -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
make x6100-gui-rebuild x6100-webserver-rebuild
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
python3 $GUI/tools/update/mkbundle.py --target $BR/build/target --manifest $(ls -d $BR/build/build/x6100-gui-*/install_manifest.txt | head -1) \
    --version $VER --out $BR/build/images/OpenX6X00-v$VER-update.tar.gz $KEYARG

echo "== Our U-Boot over buildroot's, 8 KB into the card"
dd if=$GUI/u-boot/u-boot-sunxi-with-spl.bin of=$BR/build/images/sdcard.img seek=8K bs=1 conv=notrunc
cp $BR/build/images/sdcard.img $BR/build/images/OpenX6X00-local-sdcard.img
ls -la $BR/build/images/OpenX6X00-local-sdcard.img
echo "== done"
