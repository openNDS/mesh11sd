#
# Copyright (C) 2026 Rob White <rob@blue-wave.net>
#
# This is free software, licensed under the GNU General Public License v2.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=mesh11sd-v8-experimental
PKG_VERSION:=0.0.0_pre8
PKG_RELEASE:=0

# Experimental packaging identifier only. There is no Git tag, no 8.0.0beta
# ref, and no published source tarball. PKG_HASH stays skip, matching apmond's
# development recipe, until a later authorized feed build records the hash of
# an actual tarball.
#
# OpenWrt unpacks $(DL_DIR)/$(PKG_SOURCE) with tar -C $(BUILD_DIR), so the
# archive's first path component must be $(PKG_NAME)-$(PKG_VERSION).
# scripts/build-package.py writes that archive into the private SDK dl/.
# Do not pass USE_SOURCE_DIR to make. It is global: dependency prepares,
# including json-c and lua, then symlink their build directories at this tree.
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/openNDS/mesh11sd/tar.gz/v$(PKG_VERSION)?
PKG_HASH:=skip

PKG_MAINTAINER:=Rob White <rob@blue-wave.net>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/mesh11sd-v8-experimental
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Mesh
  TITLE:=Experimental mesh11sd v8 C pure-802.11s daemon
  DEPENDS:=+libuci +libubus +libubox +libblobmsg-json
  URL:=https://github.com/openNDS/mesh11sd
endef

define Package/mesh11sd-v8-experimental/description
  Experimental IPv4 pure-802.11s package for an OpenWrt 25.12.6 image (apk)
  that already has full wpad-mbedtls and the image's default platform services.
  Those services are not extra dependencies of this package. The package is
  architecture-specific. Installation leaves the service disabled and does not
  auto-enable or start it. Loop protection is inactive. Dynamic peer promotion
  is deferred. The first hardware target is two GL-MT3600BE boards; flashed
  firmware compatibility is still to be verified.
endef

define Package/mesh11sd-v8-experimental/conffiles
/etc/config/mesh11sd_v8_runtime
endef

CMAKE_OPTIONS += -DBUILD_DAEMON=ON -DBUILD_HOST_CHECKS=OFF
CMAKE_OPTIONS += -DMESH11SD_PACKAGE_VERSION=$(PKG_VERSION)-r$(PKG_RELEASE)

# No postinst. The image and apk installer call enable/start before any
# package postinst; files/mesh11sd-v8.init refuses both in that context.

define Package/mesh11sd-v8-experimental/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mesh11sd-v8 $(1)/usr/sbin/mesh11sd-v8
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mesh11sd-v8-plan $(1)/usr/sbin/mesh11sd-v8-plan
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/mesh11sd-v8.init $(1)/etc/init.d/mesh11sd-v8-experimental
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/mesh11sd_v8_runtime.config $(1)/etc/config/mesh11sd_v8_runtime
endef

$(eval $(call BuildPackage,mesh11sd-v8-experimental))
