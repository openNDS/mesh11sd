# Host checks for the read-only planner. OpenWrt packaging is openwrt/mesh11sd.
# MESH11SD_NL80211_INCLUDE is optional and only for a host whose linux/nl80211.h
# lacks NL80211_ATTR_WIPHY_RADIOS. Example: a directory that contains linux/nl80211.h.
.PHONY: host-checks
host-checks:
	cmake -S . -B build/cmake-host -DBUILD_DAEMON=OFF -DBUILD_HOST_CHECKS=ON $(if $(MESH11SD_NL80211_INCLUDE),-DMESH11SD_NL80211_INCLUDE=$(MESH11SD_NL80211_INCLUDE),)
	cmake --build build/cmake-host
	ctest --test-dir build/cmake-host --output-on-failure
