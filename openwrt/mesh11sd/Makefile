# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2022 - 2026 BlueWave Projects and Services  <licence@blue-wave.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=mesh11sd
PKG_VERSION:=8.0.0_beta1
PKG_RELEASE:=15

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
  # uci CLI is base OpenWrt; no linked libs in chunk 1
  URL:=https://github.com/opennds/mesh11sd
endef

define Package/mesh11sd/description
  Mesh11sd autonomously manages all aspects of an 802.11s mesh network.
  Version 8 is a C port of v7 behaviour. Alpha9 matches v7 echo/printf
  sha256 digests, applies portal IPv4, and creates/configures AP vifs.
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

$(eval $(call BuildPackage,mesh11sd))
