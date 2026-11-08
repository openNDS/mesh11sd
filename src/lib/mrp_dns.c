/**
 * @file mrp_dns.c
 * @brief Build the owned authoritative DHCP range and tag text.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mrp.h"
#include <stdio.h>
#include <string.h>

static void address(uint32_t value, char text[32])
{
	snprintf(text, 32, "%u.%u.%u.%u", value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

int mesh_mrp_dns_text(const struct mesh_services *services, bool portal, char *text, size_t capacity)
{
	const struct mesh_ipv4 *pool = &services->disk.portal_pool;
	char gateway[32];
	char first[32];
	char last[32];
	char mask[32];
	uint32_t netmask;
	int length;
	if (!portal) {
		length = snprintf(text, capacity, "# mesh11sd-v8 owned runtime suppression\nno-dhcp-interface=%s\n", services->disk.bridge);
	} else {
		if (pool->prefix == 0 || pool->prefix > 30 || pool->pool_first > pool->pool_last || pool->lease_seconds == 0) {
			return -1;
		}
		netmask = UINT32_MAX << (32 - pool->prefix);
		if ((pool->address & netmask) != (pool->pool_first & netmask) ||
			(pool->address & netmask) != (pool->pool_last & netmask) ||
			(pool->address >= pool->pool_first && pool->address <= pool->pool_last)) {
			return -1;
		}
		address(pool->address, gateway);
		address(pool->pool_first, first);
		address(pool->pool_last, last);
		address(netmask, mask);
		length = snprintf(text, capacity,
			"# mesh11sd-v8 owned IPv4-only portal; allocation/probing belongs to dnsmasq\n"
			"dhcp-authoritative\n"
			"dhcp-range=set:mesh11sd_v8,%s,%s,%s,%us\n"
			"dhcp-option=tag:mesh11sd_v8,option:router,%s\n"
			"dhcp-option=tag:mesh11sd_v8,option:dns-server,%s\n"
			"host-record=mesh11sd-v8-gateway.invalid,%s\n", first, last, mask, pool->lease_seconds,
			gateway, gateway, gateway);
	}
	return length < 0 || (size_t)length >= capacity ? -1 : length;
}
