/**
 * @file mrp_payload.c
 * @brief Fill the IPv4 MRP static-gateway payload fields.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mrp.h"
#include <stdio.h>
#include <string.h>

static void address(uint32_t value, char output[32])
{
	snprintf(output, 32, "%u.%u.%u.%u", value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

int mesh_mrp_firewall_payload(struct blob_buf *buffer, const struct mesh_native_config *config,
							  const struct mesh_plan *plan)
{
	void *rules;
	void *rule;
	char prefix[48];
	char subnet[32];
	unsigned int i;
	uint32_t mask;
	if (config->mrp.profile != MESH_MRP_IPV4_ONLY || plan->ipv4.prefix == 0 || plan->ipv4.prefix > 30 ||
		config->mrp.lan_zone[0] == '\0' || config->mrp.wan_zone[0] == '\0' || config->mrp.uplink[0] == '\0') {
		return -1;
	}
	mask = UINT32_MAX << (32 - plan->ipv4.prefix);
	address(plan->ipv4.address & mask, subnet);
	snprintf(prefix, sizeof(prefix), "%s/%u", subnet, plan->ipv4.prefix);
	rules = blobmsg_open_array(buffer, "firewall");
	if (rules == NULL) {
		return -1;
	}
	for (i = 0; i < 4; i++) {
		rule = blobmsg_open_table(buffer, NULL);
		if (rule == NULL || blobmsg_add_string(buffer, "type", i == 1 ? "nat" : "rule") != 0 ||
			blobmsg_add_string(buffer, "family", "ipv4") != 0 ||
			blobmsg_add_string(buffer, "src", i == 1 ? config->mrp.wan_zone : config->mrp.lan_zone) != 0 ||
			blobmsg_add_string(buffer, "proto", i < 2 ? "all" : i == 2 ? "tcp udp" : "udp") != 0 ||
			blobmsg_add_string(buffer, "device", i < 2 ? config->mrp.uplink : config->bridge_ifname) != 0 ||
			blobmsg_add_string(buffer, "target", i == 1 ? "MASQUERADE" : "ACCEPT") != 0) {
			return -1;
		}
		if ((i < 2 && blobmsg_add_string(buffer, "src_ip", prefix) != 0) ||
			(i == 0 && blobmsg_add_string(buffer, "dest", config->mrp.wan_zone) != 0) ||
			(i != 1 && blobmsg_add_string(buffer, "direction", i == 0 ? "out" : "in") != 0) ||
			(i >= 2 && blobmsg_add_string(buffer, "dest_port", i == 2 ? "53" : "67") != 0)) {
			return -1;
		}
		blobmsg_close_table(buffer, rule);
	}
	blobmsg_close_array(buffer, rules);
	return 0;
}

