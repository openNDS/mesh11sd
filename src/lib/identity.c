/**
 * @file identity.c
 * @brief Derive the shared ULA and portal address with the legacy SHA-256 method.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mesh_config.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

bool mesh_mac_valid(const uint8_t mac[6])
{
	static const uint8_t zero[6];

	return mac != NULL && (mac[0] & 1) == 0 && memcmp(mac, zero, 6) != 0;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int mesh_mac_parse(const char *text, uint8_t mac[6])
{
	uint8_t value[6];
	size_t i;
	int high;
	int low;

	if (text == NULL || mac == NULL || strlen(text) != 17) {
		return -1;
	}
	for (i = 0; i < 6; i++) {
		high = hex_digit(text[i * 3]);
		low = hex_digit(text[i * 3 + 1]);
		if (high < 0 || low < 0 || (i < 5 && text[i * 3 + 2] != ':')) {
			return -1;
		}
		value[i] = (uint8_t)((high << 4) | low);
	}
	if (!mesh_mac_valid(value)) {
		return -1;
	}
	memcpy(mac, value, sizeof(value));
	return 0;
}

enum mesh_error mesh_identity_build(const char *group, const uint8_t factory[6],
								   const uint8_t bridge[6], struct mesh_identity *out)
{
	struct mesh_identity identity;
	char hash[65];
	char mac_line[19];
	char base[24];

	if (out == NULL || group == NULL || group[0] == '\0' || strlen(group) > 128 ||
		!mesh_mac_valid(factory) || !mesh_mac_valid(bridge)) {
		return MESH_E_IDENTITY;
	}
	memset(&identity, 0, sizeof(identity));
	memcpy(identity.factory_mac, factory, 6);
	memcpy(identity.bridge_mac, bridge, 6);
	/* v7 takes hex[9:30], then slices that at 9, 11 and 15. */
	sha256_hex((const unsigned char *)group, strlen(group), hash);
	snprintf(base, sizeof(base), "fd%.2s:%.4s:%.4s", hash + 18, hash + 20, hash + 24);
	snprintf(identity.ula_prefix, sizeof(identity.ula_prefix), "%s::/48", base);
	snprintf(mac_line, sizeof(mac_line), "%02x:%02x:%02x:%02x:%02x:%02x\n",
			 factory[0], factory[1], factory[2], factory[3], factory[4], factory[5]);
	sha256_hex((const unsigned char *)mac_line, strlen(mac_line), hash);
	memcpy(identity.lan_hint, hash + 20, 4);
	memcpy(identity.vxlan_hint, hash + 25, 4);
	snprintf(identity.lan_prefix, sizeof(identity.lan_prefix), "%s:%s::/64",
			 base, identity.lan_hint);
	snprintf(identity.portal_address, sizeof(identity.portal_address), "%s:%s::1",
			 base, identity.lan_hint);
	/* Preserve v7's bridge-derived IID, including its intentionally unflipped
	 * U/L bit. This is NOT a new RFC modified-EUI64 algorithm. */
	snprintf(identity.management_address, sizeof(identity.management_address),
			 "%s:%s:%02x%02x:%02xff:fe%02x:%02x%02x/64", base, identity.lan_hint,
			 bridge[0], bridge[1], bridge[2], bridge[3], bridge[4], bridge[5]);
	*out = identity;
	return MESH_OK;
}
