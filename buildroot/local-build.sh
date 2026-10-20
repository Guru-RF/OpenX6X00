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
make

echo "== Our U-Boot over buildroot's, 8 KB into the card"
dd if=$GUI/u-boot/u-boot-sunxi-with-spl.bin of=$BR/build/images/sdcard.img seek=8K bs=1 conv=notrunc
cp $BR/build/images/sdcard.img $BR/build/images/OpenX6X00-local-sdcard.img
ls -la $BR/build/images/OpenX6X00-local-sdcard.img
echo "== done"
