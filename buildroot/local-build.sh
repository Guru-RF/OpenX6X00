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
# The image lands in ../AetherX6100Buildroot/build/images/OpenX6X00-x6100-local-sdcard.img, the
# X6200's in ../AetherX6200Buildroot/build/images/OpenX6X00-x6200-local-sdcard.img.
# About an hour the first time; ccache in $HOME/.buildroot-ccache keeps the next ones short.
set -eo pipefail

# The two radios share this GUI (src/hw/hw.h picks the backend, cmake -DHW_BACKEND) and the
# web server, and each has its own buildroot tree from Aether Radio: the X6200's names its
# packages x6200-gui and aether-x6200-control, its board X6200
MODEL=${MODEL:-x6100}
case $MODEL in
    x6100) M=X6100; BRDEF=AetherX6100Buildroot; GUI_PKG=x6100-gui; GUI_VAR=X6100_GUI; OUT=OpenX6X00-x6100 ;;
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

# The buildroot trees are our forks, github.com/Guru-RF/AetherX6100Buildroot and .../AetherX6200Buildroot, of
# Aether Radio's (the X6100's moved from gdyuldin to OpenX6100). A change upstream reaches an image only once it
# is merged into the fork: each build says when upstream has commits the tree here has not, at the start and
# again at the end, and on GitHub as a warning on the run. It never stops the build
UPSTREAM=$([ $MODEL = x6100 ] && echo OpenX6100/AetherX6100Buildroot || echo gdyuldin/AetherX6200Buildroot)
UPSTREAM_NOTE=""
upstream_check() {
    local new
    # CI checks the tree out with one commit: its own history (small, not buildroot's) for the comparison
    if [ "$(git -C $BR rev-parse --is-shallow-repository 2>/dev/null)" = true ]; then
        git -C $BR fetch -q --unshallow 2>/dev/null || true
    fi
    if ! GIT_TERMINAL_PROMPT=0 git -C $BR fetch -q "https://github.com/$UPSTREAM" HEAD 2>/dev/null; then
        echo "== Upstream $UPSTREAM: not reachable, not checked"
        return 0
    fi
    if git -C $BR merge-base --is-ancestor FETCH_HEAD HEAD 2>/dev/null; then
        echo "== Upstream $UPSTREAM: nothing new (its HEAD $(git -C $BR rev-parse --short FETCH_HEAD) is in this tree)"
        return 0
    fi
    new=$(git -C $BR log --oneline --no-decorate HEAD..FETCH_HEAD 2>/dev/null | head -20 || true)
    UPSTREAM_NOTE=$(printf '%s\n' \
        "!! UPSTREAM CHANGED: $UPSTREAM has commits $BRDEF here has not:" \
        "$(printf '%s\n' "${new:-(none listed: see https://github.com/$UPSTREAM/commits)}" | sed 's/^/!!   /')" \
        "!! This image is built without them. Look at them, then merge them into Guru-RF/$BRDEF, or not.")
    printf '%s\n' "$UPSTREAM_NOTE"
    if [ -n "$GITHUB_ACTIONS" ]; then
        echo "::warning title=Upstream $UPSTREAM changed::$(printf '%s\n' "$new" | grep -c .) commit(s) not in Guru-RF/$BRDEF; this image is built without them (see the job summary)"
        [ -n "$GITHUB_STEP_SUMMARY" ] && printf '### Upstream %s changed\n\nNot in Guru-RF/%s, not in this image:\n\n```\n%s\n```\n' \
            "$UPSTREAM" "$BRDEF" "$new" >> "$GITHUB_STEP_SUMMARY"
    fi
    return 0
}
upstream_check

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

echo "== Pat, the Winlink client (tools/pat/build.sh): one static ARMv7 binary for both radios"
# Built with its own pinned Go (buildroot's is too old for it) and kept in $HOME/.cache/openx6x00;
# the GUI's CMake installs it as /usr/bin/pat, so it is in the image and in the update bundle
PAT_BIN=$(bash $GUI/tools/pat/build.sh)
[ -x "$PAT_BIN" ]
sed -i "/_CONF_OPTS += -DPAT_BINARY=/d" $GUI_MK
sed -i "s|^\$(eval \$(cmake-package))|${GUI_VAR}_CONF_OPTS += -DPAT_BINARY=$PAT_BIN\n&|" $GUI_MK
grep -q "^${GUI_VAR}_CONF_OPTS += -DPAT_BINARY=$PAT_BIN" $GUI_MK

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

# rootfs/ is synced into the GUI package and installed as is: a __pycache__ left there by running
# one of its scripts on the host (tests) would land in the image
find $GUI/rootfs -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null

echo "== OpenX6X00 additions to the buildroot tree (buildroot/br2_external in this repository)"
# The rtl8723bu WiFi driver package (the X6100's USB WiFi; the X6200's is on the board), the DATA
# partition repair at boot, the USB tethering profile, the Bluetooth, NTP and PulseAudio settings:
# copied over the upstream tree. Kept under board/X6100 in this repository; the X6200 tree gets
# the same files under its board
cp -a $GUI/buildroot/br2_external/package $EXT/
cp -a $GUI/buildroot/br2_external/board/X6100/linux/. $BOARD/
if [ $MODEL = x6200 ]; then
    rm -f $BOARD/rootfs-overlay/etc/modprobe.d/x6100-wifi.conf
    # module-bluez5-device with no path= in the X6200 tree's system.pa: upstream dropped it for the X6100 (ef0d395);
    # the headphones come through module-bluetooth-discover
    sed -i '/^load-module module-bluez5-device$/d' $BOARD/rootfs-overlay/etc/pulse/system.pa
    # rtw88 (the RTL8723DU WiFi driver, pinned at 461b696): the upstream TX report timeout fixes (buildroot/x6200/rtw88)
    cp $GUI/buildroot/x6200/rtw88/*.patch $EXT/package/rtw88/
    sed -i "s/BR2_EXTERNAL_X6100_PATH/BR2_EXTERNAL_X6200_PATH/" $BOARD/device_table.txt 2>/dev/null || true
fi
grep -q "^default-sample-rate = 44100" $BOARD/rootfs-overlay/etc/pulse/daemon.conf
rm -f $BOARD/rootfs-overlay/etc/init.d/S01create_data   # S00adata does it, with the repair
# S00adata and the NTP hook are in rootfs/ (installed by the GUI's CMake, so the update bundle carries
# them); the overlay's copies would be laid over them last
rm -f $BOARD/rootfs-overlay/etc/init.d/S00adata $BOARD/rootfs-overlay/etc/NetworkManager/dispatcher.d/99-ntp-sync.sh
# The WiFi and SSH backup at shutdown and restore at boot too (rootfs/etc/init.d, since v0.3.4)
rm -f $BOARD/rootfs-overlay/etc/init.d/S04restore_backup_config
# And sshd_config, keys only (rootfs/etc/ssh): the upstream overlay's lets root in with the image's password
rm -f $BOARD/rootfs-overlay/etc/ssh/sshd_config
# The card image's level for this radio (rootfs/usr/share/x6100/image-required), stamped where no update writes:
# x6100-update and the GUI compare it with what the running version wants and say when a newer image is worth
# writing
IMAGE_LEVEL=$(awk -v m=$MODEL '$1 == m && $2 ~ /^[0-9]+$/ {print $2}' $GUI/rootfs/usr/share/x6100/image-required)
mkdir -p $BOARD/rootfs-overlay/etc
printf 'model=%s\nlevel=%s\nversion=%s\n' $MODEL ${IMAGE_LEVEL:-1} \
    "$(sed -n 's/.*define VERSION.* v\([0-9.]*\).*/\1/p' $GUI/src/main.h)" > $BOARD/rootfs-overlay/etc/openx6x00-image
rm -f $BR/build/target/etc/init.d/S01create_data                         # and from an earlier build's target
# The legacy updater (any x6100_r1cbu_*.tgz on the DATA partition unpacked over / at boot, unsigned,
# unchecked): OpenX6X00 is installed as a card image, so it goes
rm -f $BOARD/rootfs-overlay/etc/init.d/S02update $BR/build/target/etc/init.d/S02update
if [ $MODEL = x6100 ]; then
    grep -q "rtl8723bu-wifi/Config.in" $EXT/Config.in || echo 'source "$BR2_EXTERNAL_X6100_PATH/package/rtl8723bu-wifi/Config.in"' >> $EXT/Config.in
    grep -q "^BR2_PACKAGE_RTL8723BU_WIFI=y" $DEFCONFIG || echo "BR2_PACKAGE_RTL8723BU_WIFI=y" >> $DEFCONFIG
    # U-Boot: mainline 2026.07 built from source with the same U-Boot defconfig (issue #21). Up to v0.3.5 the image
    # had a 2025 binary written over the 2023.01 buildroot built: it switches the DRAM supply on in an order
    # mainline put back in 2025.04 (1dadb246a3) because it hung boards of this family at the kernel start, and a
    # radio of #21 stopped there with our card and started with IU1WXY's v0.131, whose U-Boot 2026.07 is the only
    # real difference (same kernel, same device tree). The dtsi adds U-Boot's own nodes
    sed -i 's/^BR2_TARGET_UBOOT_CUSTOM_VERSION_VALUE=.*/BR2_TARGET_UBOOT_CUSTOM_VERSION_VALUE="2026.07"/' $DEFCONFIG
    grep -q "^BR2_TARGET_UBOOT_NEEDS_PYTHON3=y" $DEFCONFIG || echo "BR2_TARGET_UBOOT_NEEDS_PYTHON3=y" >> $DEFCONFIG
    sed -i 's|^\(BR2_TARGET_UBOOT_CUSTOM_DTS_PATH=".*/sun8i-r16-x6100\.dts\)"$|\1 $(BR2_EXTERNAL_X6100_PATH)/board/X6100/u-boot/sun8i-r16-x6100-u-boot.dtsi"|' $DEFCONFIG
    cp $GUI/buildroot/br2_external/board/X6100/u-boot/sun8i-r16-x6100-u-boot.dtsi $EXT/board/X6100/u-boot/
    # buildroot 2023.02 copies COPYING over Licenses/gpl-2.0.txt for U-Boot before 2013.10; in 2026.07 COPYING is a
    # link to that file and the copy fails the extraction
    sed -i 's#if \[ -f \$(@D)/COPYING \]; then#if [ -f $(@D)/COPYING ] \&\& [ ! -L $(@D)/COPYING ]; then#' $BR/buildroot/boot/uboot/uboot.mk
    # and builds mkeficapsule (EFI capsules, which the radio does not use) with the build host's gnutls, which the
    # container has not: a host tool only, the U-Boot binary is the same without it
    grep -q "CONFIG_TOOLS_MKEFICAPSULE" $EXT/board/X6100/u-boot/sun8i-r16-x6100_defconfig \
        || echo "# CONFIG_TOOLS_MKEFICAPSULE is not set" >> $EXT/board/X6100/u-boot/sun8i-r16-x6100_defconfig
    grep -E "^BR2_TARGET_UBOOT_(CUSTOM_VERSION_VALUE|NEEDS_PYTHON3|CUSTOM_DTS_PATH)" $DEFCONFIG
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
if [ $MODEL = x6100 ]; then
    # A U-Boot configured here before its defconfig turned mkeficapsule off is configured again
    grep -qs "^CONFIG_TOOLS_MKEFICAPSULE=y" $BR/build/build/uboot-2026.07/.config && make uboot-dirclean
    # buildroot checks no hash for a U-Boot version of one's own: this one is the release's
    # (ftp.denx.de, the same file as in IU1WXY's v0.131 sources)
    make uboot-source
    echo "78e8bfc382fe388f9b55aa1daf8c563522a037779b5d4c349d1415e381f1243e  $BR/buildroot/dl/uboot/u-boot-2026.07.tar.bz2" | sha256sum -c -
fi
make lame mpg123
make libsndfile-dirclean
# A package patched after an earlier build here is built again: buildroot applies patches at extraction only
if [ $MODEL = x6200 ] && ls -d $BR/build/build/rtw88-*/ > /dev/null 2>&1 \
        && ! grep -qs "0001-wifi-rtw88-TX-report" $BR/build/build/rtw88-*/.applied_patches_list; then
    make rtw88-dirclean
fi
if [ $MODEL = x6100 ] && ls -d $BR/build/build/rtl8723bu-wifi-*/ > /dev/null 2>&1 \
        && ! grep -qs "0005-ht-capabilities-element" $BR/build/build/rtl8723bu-wifi-*/.applied_patches_list; then
    make rtl8723bu-wifi-dirclean
fi
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
        find "$d" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null   # emptied above, installed as a directory otherwise
    done
    find $BR/build/target/usr/sbin -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
    # The web server the same way: its synced sources lose what webserver/ no longer has (a tree
    # built stock first kept the stock web server's files, which went into the wheel), and it is
    # reinstalled whole below, so what an earlier version installed goes first (the module, its
    # dist-info and the script in bin, which the installer refuses to overwrite)
    for d in $BR/build/build/x6100-webserver-*/src; do
        [ -d "$d" ] || continue
        (cd "$d" && find . -type f -not -path '*/__pycache__/*') | while read -r f; do
            [ -e "$GUI/webserver/src/$f" ] || { echo "   stale web server file $f"; rm -f "$d/$f"; }
        done
    done
    rm -rf $BR/build/target/usr/lib/python3*/site-packages/x6100_webserver* $BR/build/target/usr/bin/x6100_webserver
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
# What a tree built stock before installed in target/ stays there (buildroot never removes what a
# package put there) and would go into the image: the PCI firmware whose options were dropped above
# (16 MB; RTL_815X, kept, is the rtl815x files of rtl_nic, the rest came with RTL_8169), RHVoice
# (dropped above, 13 MB), the stock GUI's binary and data (/usr/sbin/x6200_gui, /usr/share/x6200)
rm -rf $BR/build/target/lib/firmware/{bnx2x,cxgb4,e100,intel,microchip,qed,tigon}
find $BR/build/target/lib/firmware/rtl_nic -type f ! -name 'rtl815*' -delete 2>/dev/null
rm -rf $BR/build/target/usr/share/RHVoice $BR/build/target/usr/lib/libRHVoice* $BR/build/target/usr/bin/RHVoice*
rm -rf $BR/build/target/usr/sbin/x6200_gui $BR/build/target/usr/share/x6200
# and the RS theme the GUI installed up to v0.3.5 (cut from an Icom RS-BA1 screenshot: not shipped any more)
rm -rf $BR/build/target/usr/share/x6100/themes
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

cp $BR/build/images/sdcard.img $BR/build/images/$OUT-local-sdcard.img
ls -la $BR/build/images/$OUT-local-sdcard.img
[ -n "$UPSTREAM_NOTE" ] && printf '%s\n' "$UPSTREAM_NOTE"     # again, after the long log
echo "== done"
