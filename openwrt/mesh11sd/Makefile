# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2022 - 2026 BlueWave Projects and Services  <licence@blue-wave.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=mesh11sd
PKG_VERSION:=8.0.0_beta1
PKG_RELEASE:=81

PKG_MAINTAINER:=Rob White <rob@blue-wave.net>
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=LICENSE

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/opennds/mesh11sd/tar.gz/v$(PKG_VERSION)?
PKG_HASH:=skip

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/mesh11sd
  SUBMENU:=Mesh
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Dynamic 802.11s Mesh Management Daemon
  DEPENDS:=+kmod-nft-bridge +libubus +libubox +libblobmsg-json
  URL:=https://github.com/opennds/mesh11sd
endef

define Package/mesh11sd/description
  Mesh11sd autonomously configures and manages 802.11s backhaul, access
  points, bridging, portal detection and tunnelled networks on OpenWrt.
endef

define Package/mesh11sd/conffiles
/etc/config/mesh11sd
endef

CMAKE_OPTIONS += -DMESH11SD_PACKAGE_VERSION=$(PKG_VERSION)

define Package/mesh11sd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mesh11sd $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/mesh11sd $(1)/etc/config/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/mesh11sd $(1)/etc/init.d/
endef

define Package/mesh11sd/postinst
#!/bin/sh
# v7 with auto_config moved the luci-base cgi to luci.original (mode 444)
# and replaced it with a "LuCI is currently DISABLED" page. Undo that once
# on install or upgrade: put the original script back and drop the backup.
[ -n "$${IPKG_INSTROOT}" ] && exit 0
if [ -f /www/cgi-bin/luci.original ]; then
	mv /www/cgi-bin/luci.original /www/cgi-bin/luci
	chmod 755 /www/cgi-bin/luci
	rm -rf /tmp/luci-*
fi
exit 0
endef

$(eval $(call BuildPackage,mesh11sd))
