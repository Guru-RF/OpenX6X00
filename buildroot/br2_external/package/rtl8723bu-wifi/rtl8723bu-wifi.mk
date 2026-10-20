################################################################################
#
# rtl8723bu-wifi: lwfinger's out-of-tree Realtek driver (8723bu.ko),
# used on the X6100 instead of the in-kernel rtl8xxxu
#
################################################################################

RTL8723BU_WIFI_VERSION = af3a408d6399655b0db23c2c8720436ca725ca47
RTL8723BU_WIFI_SITE = $(call github,lwfinger,rtl8723bu,$(RTL8723BU_WIFI_VERSION))
RTL8723BU_WIFI_LICENSE = GPL-2.0

# Firmware is built into the module; nothing to install besides 8723bu.ko
RTL8723BU_WIFI_MODULE_MAKE_OPTS = \
	KVER=$(LINUX_VERSION_PROBED) \
	KSRC=$(LINUX_DIR)

$(eval $(kernel-module))
$(eval $(generic-package))
