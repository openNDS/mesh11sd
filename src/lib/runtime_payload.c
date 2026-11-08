/**
 * @file runtime_payload.c
 * @brief Build fixed-capacity procd wifi and network records.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "runtime.h"
#include "mrp.h"

#include <string.h>
#include <stdio.h>
#include <libubox/blobmsg.h>

void mesh_runtime_data_clear(struct mesh_runtime_data *data)
{
	if (data != NULL) {
		mesh_sensitive_clear(data, sizeof(*data));
	}
}

static void blob_clear(struct blob_buf *buffer)
{
	if (buffer->buf != NULL) {
		mesh_sensitive_clear(buffer->buf, buffer->buflen);
	}
	memset(buffer, 0, sizeof(*buffer));
}

static bool no_growth(struct blob_buf *buffer, int minimum)
{
	(void)buffer;
	(void)minimum;
	return false;
}

enum mesh_error mesh_runtime_payload(const struct mesh_native_config *config,
									 const struct mesh_plan *plan,
									 struct mesh_runtime_data *output)
{
	struct blob_buf buffer;
	unsigned char storage[MESH_RUNTIME_BYTES];
	struct mesh_runtime_data result;
	void *type;
	void *radio;
	void *vif;
	void *settings;
	void *interface;
	void *addresses;
	void *networks;
	size_t length;
	char channel[4];
	int written;
	enum mesh_error error;

	if (config == NULL || plan == NULL || output == NULL || !config->enabled ||
		strcmp(config->on_air_id, plan->mesh_id) != 0 || strcmp(config->phy, plan->phy) != 0 ||
		strcmp(config->mesh_ifname, plan->mesh_ifname) != 0 ||
		strcmp(config->bridge_ifname, plan->bridge_ifname) != 0 ||
		config->config.frequency_mhz != plan->frequency_mhz ||
		config->radio_channel == 0 || config->radio_channel > 233 ||
		strlen(config->credential) != 64 || plan->security != MESH_SECURITY_SAE) {
		return MESH_E_INPUT;
	}
	if (plan->role != MESH_ROLE_PEER &&
		!(plan->role == MESH_ROLE_MRP && config->manage_services && config->mrp.profile == MESH_MRP_IPV4_ONLY)) {
		/* MRP needs native DHCP/DNS/RA/forwarding, not a half-configured portal. */
		return MESH_E_ROLE_PENDING;
	}
	written = snprintf(channel, sizeof(channel), "%u", config->radio_channel);
	if (written < 0 || (size_t)written >= sizeof(channel)) {
		return MESH_E_INPUT;
	}
	memset(&buffer, 0, sizeof(buffer));
	memset(&result, 0, sizeof(result));
	/* Fixed storage prevents realloc from releasing unwiped secret copies. */
	memset(storage, 0, sizeof(storage));
	buffer.buf = storage;
	buffer.buflen = sizeof(storage);
	buffer.grow = no_growth;
	error = MESH_E_PLATFORM;
	if (blob_buf_init(&buffer, 0) != 0) {
		goto failed;
	}
	type = blobmsg_open_table(&buffer, "wifi-device");
	if (type == NULL) {
		goto failed;
	}
	radio = blobmsg_open_table(&buffer, config->radio_section);
	if (radio == NULL) {
		goto failed;
	}
	if (blobmsg_add_u8(&buffer, "disabled", false) != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "channel", channel) != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "htmode", config->htmode) != 0) {
		goto failed;
	}
	if (config->country[0] != '\0') {
		if (blobmsg_add_string(&buffer, "country", config->country) != 0) {
			goto failed;
		}
	}
	blobmsg_close_table(&buffer, radio);
	blobmsg_close_table(&buffer, type);
	type = blobmsg_open_table(&buffer, "wifi-iface");
	if (type == NULL) {
		goto failed;
	}
	radio = blobmsg_open_table(&buffer, config->radio_section);
	if (radio == NULL) {
		goto failed;
	}
	vif = blobmsg_open_table(&buffer, "mesh11sd_v8_mesh");
	if (vif == NULL) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "device", config->radio_section) != 0) {
		goto failed;
	}
	settings = blobmsg_open_table(&buffer, "config");
	if (settings == NULL) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "mode", "mesh") != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "ifname", plan->mesh_ifname) != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "mesh_id", plan->mesh_id) != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "encryption", "sae") != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "key", config->credential) != 0) {
		goto failed;
	}
	if (blobmsg_add_u8(&buffer, "disabled", config->hold_mesh) != 0) {
		goto failed;
	}
	networks = blobmsg_open_array(&buffer, "network");
	if (networks == NULL) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, NULL, config->network) != 0) {
		goto failed;
	}
	blobmsg_close_array(&buffer, networks);
	if (blobmsg_add_u32(&buffer, "mesh_hwmp_rootmode", config->mobility.rootmode) != 0) {
		goto failed;
	}
	if (config->mobility.tuned) {
		if (blobmsg_add_u32(&buffer, "mesh_rssi_threshold", (uint32_t)config->mobility.rssi_threshold) != 0 ||
			blobmsg_add_u32(&buffer, "mesh_hwmp_rann_interval", config->mobility.rann_interval) != 0 ||
			blobmsg_add_u32(&buffer, "mesh_hwmp_preq_min_interval", config->mobility.preq_min_interval) != 0 ||
			blobmsg_add_u32(&buffer, "mesh_hwmp_active_path_timeout", config->mobility.active_path_timeout) != 0 ||
			blobmsg_add_u32(&buffer, "mesh_max_peer_links", config->mobility.max_peer_links) != 0 ||
			blobmsg_add_u32(&buffer, "mesh_hwmp_max_preq_retries", config->mobility.max_preq_retries) != 0) {
			goto failed;
		}
	}
	blobmsg_close_table(&buffer, settings);
	blobmsg_close_table(&buffer, vif);
	blobmsg_close_table(&buffer, radio);
	blobmsg_close_table(&buffer, type);
	type = blobmsg_open_table(&buffer, "network-interface");
	if (type == NULL) {
		goto failed;
	}
	interface = blobmsg_open_table(&buffer, config->network);
	if (interface == NULL) {
		goto failed;
	}
	if (config->manage_services && blobmsg_add_u8(&buffer, "force_link", true) != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "proto", config->manage_services ? "none" : "dhcp") != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "device", config->bridge_ifname) != 0) {
		goto failed;
	}
	if (blobmsg_add_u8(&buffer, "ipv6", true) != 0) {
		goto failed;
	}
	blobmsg_close_table(&buffer, interface);
	if (config->manage_services) {
		/* Keep LAN's service data on a stable UP base interface. The ONE
		 * netifd DHCP client is a private alias; lease loss/renew cannot flush
		 * LAN's scoped odhcpd mode override. No second DHCP client is spawned. */
		interface = blobmsg_open_table(&buffer, "mesh11sd_v8_client");
		if (interface == NULL || blobmsg_add_string(&buffer, "proto", plan->role == MESH_ROLE_MRP ? "static" : "dhcp") != 0 ||
			blobmsg_add_string(&buffer, "device", config->bridge_ifname) != 0 ||
			blobmsg_add_u8(&buffer, "auto", false) != 0 || blobmsg_add_u8(&buffer, "ipv6", true) != 0) {
			goto failed;
		}
		if (plan->role == MESH_ROLE_MRP) {
			char address[32];
			void *ipv4;
			snprintf(address, sizeof(address), "%u.%u.%u.%u/%u", plan->ipv4.address >> 24,
					 (plan->ipv4.address >> 16) & 255, (plan->ipv4.address >> 8) & 255,
					 plan->ipv4.address & 255, plan->ipv4.prefix);
			ipv4 = blobmsg_open_array(&buffer, "ipaddr");
			if (ipv4 == NULL || blobmsg_add_string(&buffer, NULL, address) != 0) {
				goto failed;
			}
			blobmsg_close_array(&buffer, ipv4);
			if (blobmsg_add_u8(&buffer, "defaultroute", false) != 0 ||
				blobmsg_add_u8(&buffer, "delegate", false) != 0) {
				goto failed;
			}
		}
		blobmsg_close_table(&buffer, interface);
	}
	interface = blobmsg_open_table(&buffer, "mesh11sd_v8_management");
	if (interface == NULL) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "proto", "static") != 0) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, "device", config->bridge_ifname) != 0) {
		goto failed;
	}
	addresses = blobmsg_open_array(&buffer, "ip6addr");
	if (addresses == NULL) {
		goto failed;
	}
	if (blobmsg_add_string(&buffer, NULL, plan->identity.management_address) != 0) {
		goto failed;
	}
	if (plan->role == MESH_ROLE_MRP) {
		char portal[64];
		snprintf(portal, sizeof(portal), "%s/64", plan->identity.portal_address);
		if (blobmsg_add_string(&buffer, NULL, portal) != 0) {
			goto failed;
		}
	}
	blobmsg_close_array(&buffer, addresses);
	if (blobmsg_add_u8(&buffer, "defaultroute", false) != 0) {
		goto failed;
	}
	if (blobmsg_add_u8(&buffer, "delegate", false) != 0) {
		goto failed;
	}
	blobmsg_close_table(&buffer, interface);
	blobmsg_close_table(&buffer, type);
	if (plan->role == MESH_ROLE_MRP && mesh_mrp_firewall_payload(&buffer, config, plan) != 0) {
		goto failed;
	}
	error = MESH_E_PLATFORM;
	if (buffer.head != NULL) {
		length = blob_pad_len(buffer.head);
		if (length <= sizeof(result.bytes)) {
			memcpy(result.bytes, buffer.head, length);
			result.length = length;
			*output = result;
			error = MESH_OK;
		}
	}
failed:
	blob_clear(&buffer);
	mesh_runtime_data_clear(&result);
	return error;
}
