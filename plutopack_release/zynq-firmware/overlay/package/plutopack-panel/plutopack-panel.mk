################################################################################
#
# plutopack-panel
#
################################################################################
PLUTOPACK_PANEL_VERSION = 1.0
PLUTOPACK_PANEL_SITE = $(BR2_EXTERNAL_PLUTOSDR_PATH)/package/plutopack-panel/src
PLUTOPACK_PANEL_SITE_METHOD = local

define PLUTOPACK_PANEL_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -O2 -Wall" -C $(@D)
endef

define PLUTOPACK_PANEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/plutopack-panel $(TARGET_DIR)/usr/bin/plutopack-panel
	$(INSTALL) -D -m 0755 $(@D)/spitest_zynq   $(TARGET_DIR)/usr/bin/spitest_zynq
	$(INSTALL) -D -m 0755 $(@D)/pp-gui         $(TARGET_DIR)/usr/bin/pp-gui
	$(INSTALL) -D -m 0755 $(@D)/pp-launcher    $(TARGET_DIR)/usr/bin/pp-launcher
	$(INSTALL) -D -m 0755 $(@D)/pp-test        $(TARGET_DIR)/usr/bin/pp-test
	$(INSTALL) -D -m 0755 $(@D)/pp-touchcal    $(TARGET_DIR)/usr/bin/pp-touchcal
	$(INSTALL) -D -m 0755 $(@D)/pp-sdr         $(TARGET_DIR)/usr/bin/pp-sdr
	$(INSTALL) -D -m 0755 $(@D)/pp-tonegen     $(TARGET_DIR)/usr/bin/pp-tonegen
	$(INSTALL) -D -m 0755 $(@D)/pp-spectrum    $(TARGET_DIR)/usr/bin/pp-spectrum
	$(INSTALL) -D -m 0755 $(@D)/pp-sna         $(TARGET_DIR)/usr/bin/pp-sna
	$(INSTALL) -D -m 0755 $(@D)/pp-scope       $(TARGET_DIR)/usr/bin/pp-scope
	$(INSTALL) -D -m 0755 $(@D)/pp-iqrec       $(TARGET_DIR)/usr/bin/pp-iqrec
	$(INSTALL) -D -m 0755 $(@D)/pp-iqfeed      $(TARGET_DIR)/usr/bin/pp-iqfeed
	$(INSTALL) -D -m 0755 $(@D)/pp-rtl433      $(TARGET_DIR)/usr/bin/pp-rtl433
	$(INSTALL) -D -m 0755 $(@D)/pp-adsb        $(TARGET_DIR)/usr/bin/pp-adsb
	$(INSTALL) -D -m 0755 $(@D)/pp-vna         $(TARGET_DIR)/usr/bin/pp-vna
	$(INSTALL) -D -m 0755 $(@D)/pp-twotone     $(TARGET_DIR)/usr/bin/pp-twotone
	$(INSTALL) -D -m 0755 $(@D)/pp-ampgain     $(TARGET_DIR)/usr/bin/pp-ampgain
	$(INSTALL) -D -m 0755 $(@D)/pp-atten       $(TARGET_DIR)/usr/bin/pp-atten
	$(INSTALL) -D -m 0755 $(@D)/pp-manual      $(TARGET_DIR)/usr/bin/pp-manual
	$(INSTALL) -D -m 0755 $(@D)/pp-tetris      $(TARGET_DIR)/usr/bin/pp-tetris
	$(INSTALL) -D -m 0755 $(@D)/pp-acks        $(TARGET_DIR)/usr/bin/pp-acks
	$(INSTALL) -D -m 0755 $(@D)/pp-rficstat    $(TARGET_DIR)/usr/bin/pp-rficstat
	$(INSTALL) -D -m 0755 $(@D)/pp-term        $(TARGET_DIR)/usr/bin/pp-term
	$(INSTALL) -D -m 0755 $(@D)/pp-status      $(TARGET_DIR)/usr/bin/pp-status
	$(INSTALL) -D -m 0755 $(@D)/pp-probe       $(TARGET_DIR)/usr/bin/pp-probe
	$(INSTALL) -D -m 0755 $(@D)/pp-fbnoise     $(TARGET_DIR)/usr/bin/pp-fbnoise
	$(INSTALL) -D -m 0755 $(@D)/disp_speed.sh  $(TARGET_DIR)/usr/bin/disp_speed
	$(INSTALL) -D -m 0755 $(@D)/pp-fbrebind    $(TARGET_DIR)/usr/bin/pp-fbrebind
	$(INSTALL) -D -m 0755 $(@D)/pp-demo        $(TARGET_DIR)/usr/bin/pp-demo
	$(INSTALL) -D -m 0644 $(@D)/README.pp      $(TARGET_DIR)/usr/share/plutopack/README
	$(INSTALL) -D -m 0755 $(@D)/S99plutopack-panel $(TARGET_DIR)/etc/init.d/S30plutopack-panel
endef

$(eval $(generic-package))
