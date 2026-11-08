/**
 * @file peer_observe.c
 * @brief Verify mesh, AP, supplicant, DHCP, and bridge read-back.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "peer.h"
#include "services.h"
#include "mrp.h"
#include <arpa/inet.h>
#include <linux/nl80211.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool string_is(struct blob_attr *parent, const char *name, const char *expected)
{
	struct blob_attr *field = mesh_field(parent, name, BLOBMSG_TYPE_STRING);
	return field != NULL && strcmp(blobmsg_get_string(field), expected) == 0;
}

static bool boolean_is(struct blob_attr *parent, const char *name, bool expected)
{
	struct blob_attr *field = mesh_field(parent, name, BLOBMSG_TYPE_BOOL);
	return field != NULL && blobmsg_get_bool(field) == expected;
}

static int wireless(struct mesh_peer *peer, const struct mesh_native_config *config,
					struct mesh_runtime_data *reply)
{
	struct mesh_message request;
	int result = -1;
	if (mesh_message_init(&request) == 0 &&
		blobmsg_add_string(&request.blob, "device", config->radio_section) == 0) {
		result = mesh_peer_call(peer, "network.wireless", "status", &request, reply);
	}
	mesh_message_clear(&request);
	return result;
}

static struct blob_attr *section(struct blob_attr *radio, const char *name)
{
	struct blob_attr *interfaces = mesh_field(radio, "interfaces", BLOBMSG_TYPE_ARRAY);
	struct blob_attr *item;
	struct blob_attr *found = NULL;
	int remaining;
	if (interfaces == NULL) {
		return NULL;
	}
	blobmsg_for_each_attr(item, interfaces, remaining) {
		if (blobmsg_type(item) == BLOBMSG_TYPE_TABLE && string_is(item, "section", name)) {
			if (found != NULL) {
				return NULL;
			}
			found = item;
		}
	}
	return found;
}

static uint32_t phy_index(const struct mesh_native_config *config)
{
	return (uint32_t)strtoul(config->phy + 3, NULL, 10);
}

enum mesh_error mesh_peer_radio(void *context, const struct mesh_native_config *config, struct mesh_radio *output)
{
	struct mesh_peer *peer = context;
	struct mesh_runtime_data reply;
	struct mesh_interface_list interfaces;
	struct mesh_radio radio;
	struct blob_attr *status;
	struct blob_attr *ap;
	struct blob_attr *field;
	struct mesh_interface *interface;
	char names[MESH_APS_MAX][16];
	uint32_t beacons[MESH_APS_MAX + 1];
	uint32_t index;
	size_t i;
	size_t j;
	bool managed;
	enum mesh_error error = MESH_E_RADIO;

	memset(&reply, 0, sizeof(reply));
	memset(names, 0, sizeof(names));
	if (peer == NULL || config == NULL || output == NULL || config->configured_ap_count > MESH_APS_MAX) {
		return MESH_E_INPUT;
	}
	index = phy_index(config);
	if (peer->transport.wiphy(peer->transport.context, index, config->config.frequency_mhz, &radio) != MESH_OK ||
		peer->transport.interfaces(peer->transport.context, &interfaces) != MESH_OK ||
		wireless(peer, config, &reply) != 0 || !mesh_reply_valid(&reply)) {
		goto done;
	}
	status = mesh_root_field(&reply, config->radio_section, BLOBMSG_TYPE_TABLE);
	if (status == NULL) {
		goto done;
	}
	beacons[0] = config->radio_beacon_interval;
	for (i = 0; i < config->configured_ap_count; i++) {
		/* mac80211.sh uses one wifi-device beacon_int for AP and mesh. */
		beacons[i + 1] = beacons[0];
		ap = section(status, config->aps[i].section);
		field = mesh_field(ap, "ifname", BLOBMSG_TYPE_STRING);
		if (field != NULL) {
			if (strlen(blobmsg_get_string(field)) >= sizeof(names[i])) {
				goto done;
			}
			memcpy(names[i], blobmsg_get_string(field), strlen(blobmsg_get_string(field)) + 1);
		} else if (config->aps[i].ifname[0] != '\0') {
			memcpy(names[i], config->aps[i].ifname, sizeof(names[i]));
		}
	}
	if (!mesh_combination_allows(&radio, config->configured_ap_count, beacons,
								 config->configured_ap_count + 1)) {
		goto done;
	}
	for (i = 0; i < interfaces.count; i++) {
		interface = &interfaces.entries[i];
		if (interface->wiphy != index) {
			continue;
		}
		if (peer->present && interface->iftype == NL80211_IFTYPE_MESH_POINT &&
			strcmp(interface->ifname, config->mesh_ifname) == 0) {
			continue;
		}
		managed = false;
		for (j = 0; j < config->configured_ap_count; j++) {
			if (interface->iftype == NL80211_IFTYPE_AP && names[j][0] != '\0' &&
				strcmp(interface->ifname, names[j]) == 0) {
				managed = true;
				break;
			}
		}
		if (!managed) {
			goto done;
		}
	}
	if (!peer->have_baseline) {
		peer->baseline = interfaces;
		peer->have_baseline = true;
	}
	radio.combination_verified = true;
	*output = radio;
	error = MESH_OK;
done:
	mesh_runtime_data_clear(&reply);
	return error;
}

int mesh_peer_preflight(struct mesh_peer *peer, const struct mesh_native_config *config)
{
	struct mesh_message request;
	struct mesh_runtime_data reply;
	struct blob_attr *service;
	struct blob_attr *instances;
	struct blob_attr *instance;
	char object[96];
	int remaining;
	int status;
	int result = -1;

	memset(&reply, 0, sizeof(reply));
	if (!config->peer_services_ready && peer->services == NULL) {
		return -1;
	}
	if (mesh_message_init(&request) != 0 || blobmsg_add_string(&request.blob, "name", "mesh11sd") != 0 ||
		mesh_peer_call(peer, "service", "list", &request, &reply) != 0 || !mesh_reply_valid(&reply)) {
		goto done;
	}
	service = mesh_root_field(&reply, "mesh11sd", BLOBMSG_TYPE_TABLE);
	if (service == NULL && blob_len((struct blob_attr *)reply.bytes) != 0) {
		goto done;
	}
	instances = mesh_field(service, "instances", -1);
	if (instances != NULL && blobmsg_type(instances) != BLOBMSG_TYPE_TABLE) {
		goto done;
	}
	if (instances != NULL) {
		blobmsg_for_each_attr(instance, instances, remaining) {
			if (blobmsg_type(instance) != BLOBMSG_TYPE_TABLE || !boolean_is(instance, "running", false)) {
				goto done;
			}
		}
	}
	if (!peer->present && !peer->recovery_cleanup) {
		mesh_message_clear(&request);
		mesh_message_init(&request);
		snprintf(object, sizeof(object), "network.interface.mesh11sd_v8_management");
		mesh_runtime_data_clear(&reply);
		status = peer->transport.invoke(peer->transport.context, object, "status", request.blob.head, &reply);
		if (status != 4) {
			goto done;
		}
		if (config->manage_services) {
			mesh_runtime_data_clear(&reply);
			status = peer->transport.invoke(peer->transport.context, "network.interface.mesh11sd_v8_client",
											 "status", request.blob.head, &reply);
			if (status != 4) {
				goto done;
			}
		}
	}
	peer->config = config;
	result = 0;
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static bool untouched(struct mesh_peer *peer, const struct mesh_interface_list *current)
{
	size_t i;
	size_t j;
	const struct mesh_interface *old;
	const struct mesh_interface *now;
	bool found;
	if (!peer->have_baseline) {
		return false;
	}
	for (i = 0; i < peer->baseline.count; i++) {
		old = &peer->baseline.entries[i];
		if (old->wiphy == phy_index(peer->config)) {
			continue;
		}
		found = false;
		for (j = 0; j < current->count; j++) {
			now = &current->entries[j];
			if (old->ifindex == now->ifindex && old->wdev == now->wdev && old->wiphy == now->wiphy && old->iftype == now->iftype &&
				old->frequency == now->frequency && strcmp(old->ifname, now->ifname) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static bool has_kernel_interface(const struct mesh_interface_list *list, const char *name,
								 uint32_t phy, uint32_t type, uint32_t frequency)
{
	size_t i;
	for (i = 0; i < list->count; i++) {
		if (strcmp(list->entries[i].ifname, name) == 0 && list->entries[i].wiphy == phy &&
			list->entries[i].iftype == type && list->entries[i].frequency == frequency) {
			return true;
		}
	}
	return false;
}

static int verify_once(struct mesh_peer *peer, const struct mesh_plan *plan)
{
	const struct mesh_native_config *config = peer->config;
	struct mesh_message request;
	struct mesh_runtime_data reply;
	struct mesh_interface_list interfaces;
	struct blob_attr *status;
	struct blob_attr *vif;
	struct blob_attr *settings;
	struct blob_attr *field;
	char object[96];
	size_t i;
	int result = -1;

	memset(&reply, 0, sizeof(reply));
	mesh_message_init(&request);
	if (peer->transport.interfaces(peer->transport.context, &interfaces) != MESH_OK ||
		!untouched(peer, &interfaces) ||
		!has_kernel_interface(&interfaces, plan->mesh_ifname, phy_index(config), NL80211_IFTYPE_MESH_POINT,
							  plan->frequency_mhz) || wireless(peer, config, &reply) != 0) {
		goto done;
	}
	status = mesh_root_field(&reply, config->radio_section, BLOBMSG_TYPE_TABLE);
	if (!boolean_is(status, "up", true) || !boolean_is(status, "pending", false) ||
		!boolean_is(status, "disabled", false)) {
		goto done;
	}
	vif = section(status, "mesh11sd_v8_mesh");
	settings = mesh_field(vif, "config", BLOBMSG_TYPE_TABLE);
	if (!string_is(vif, "ifname", plan->mesh_ifname) || !string_is(settings, "mode", "mesh") ||
		!string_is(settings, "mesh_id", plan->mesh_id) || !string_is(settings, "encryption", "sae")) {
		goto done;
	}
	for (i = 0; i < config->configured_ap_count; i++) {
		vif = section(status, config->aps[i].section);
		field = mesh_field(vif, "ifname", BLOBMSG_TYPE_STRING);
		if (field == NULL || !has_kernel_interface(&interfaces, blobmsg_get_string(field), phy_index(config),
													NL80211_IFTYPE_AP, plan->frequency_mhz)) {
			goto done;
		}
	}
	mesh_runtime_data_clear(&reply);
	if (blobmsg_add_string(&request.blob, "name", plan->mesh_ifname) != 0 ||
		mesh_peer_call(peer, "wpa_supplicant", "iface_status", &request, &reply) != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "state", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), "COMPLETED") != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "frequency", BLOBMSG_TYPE_INT32);
	if (field == NULL || blobmsg_get_u32(field) != plan->frequency_mhz) {
		goto done;
	}
	mesh_message_clear(&request);
	mesh_message_init(&request);
	snprintf(object, sizeof(object), "network.interface.%s", config->network);
	if (mesh_peer_call(peer, object, "status", &request, &reply) != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "proto", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), config->manage_services ? "none" : "dhcp") != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), plan->bridge_ifname) != 0 ||
		peer->transport.verify_bridge(peer->transport.context, plan) != 0) {
		goto done;
	}
	if (config->manage_services) {
		if (mesh_peer_call(peer, "network.interface.mesh11sd_v8_client", "status", &request, &reply) != 0) {
			goto done;
		}
		field = mesh_root_field(&reply, "proto", BLOBMSG_TYPE_STRING);
		if (field == NULL || strcmp(blobmsg_get_string(field), plan->role == MESH_ROLE_MRP ? "static" : "dhcp") != 0) {
			goto done;
		}
		field = mesh_root_field(&reply, "autostart", BLOBMSG_TYPE_BOOL);
		if (field == NULL || !blobmsg_get_bool(field)) {
			goto done;
		}
		field = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
		if (field == NULL || strcmp(blobmsg_get_string(field), plan->bridge_ifname) != 0 ||
			mesh_services_verify(peer) != 0 || (plan->role == MESH_ROLE_MRP && mesh_mrp_verify(peer) != 0)) {
			goto done;
		}
	}
	snprintf(object, sizeof(object), "network.interface.mesh11sd_v8_management");
	if (mesh_peer_call(peer, object, "status", &request, &reply) != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "proto", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), "static") != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
	if (field == NULL || !blobmsg_get_bool(field)) {
		goto done;
	}
	field = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), plan->bridge_ifname) != 0) {
		goto done;
	}
	result = 0;
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int verify_held_once(struct mesh_peer *peer, const struct mesh_plan *plan)
{
	struct mesh_interface_list held;
	size_t index;
	if (peer->transport.interfaces(peer->transport.context, &held) != MESH_OK || !untouched(peer, &held)) {
		return -1;
	}
	for (index = 0; index < held.count; index++) {
		if (strcmp(held.entries[index].ifname, plan->mesh_ifname) == 0) {
			return -1;
		}
	}
	return 0;
}

int mesh_peer_verify(void *context, const struct mesh_plan *plan)
{
	struct mesh_peer *peer = context;
	uint64_t deadline;
	unsigned int attempts = 0;
	if (peer->config == NULL || plan == NULL || (plan->role != MESH_ROLE_PEER &&
		 !(plan->role == MESH_ROLE_MRP && peer->config->mrp.profile == MESH_MRP_IPV4_ONLY))) {
		return -1;
	}
	peer->plan = plan;
	deadline = peer->transport.now_ms(peer->transport.context) + 30000;
	do {
		if (peer->cancelled != NULL && *peer->cancelled) {
			return -1;
		}
		if ((peer->config->hold_mesh ? verify_held_once(peer, plan) : verify_once(peer, plan)) == 0) {
			return 0;
		}
		if (peer->transport.now_ms(peer->transport.context) >= deadline) {
			break;
		}
		peer->transport.wait_ms(peer->transport.context, 500);
	} while (++attempts < 61);
	return -1;
}

int mesh_peer_reload(void *context)
{
	struct mesh_peer *peer = context;
	struct mesh_message request;
	struct mesh_runtime_data reply;
	struct mesh_interface_list interfaces;
	uint64_t deadline;
	unsigned int attempts = 0;
	size_t i;
	bool absent;
	int result = -1;

	memset(&reply, 0, sizeof(reply));
	if (mesh_message_init(&request) != 0 || mesh_peer_call(peer, "network", "reload", &request, &reply) != 0) {
		goto done;
	}
	if (peer->config != NULL && peer->config->mrp.profile == MESH_MRP_IPV4_ONLY &&
		(peer->transport.reload_firewall == NULL || peer->transport.reload_firewall(peer->transport.context) != 0 ||
		 (!peer->present && mesh_mrp_firewall_verify(peer, true) != 0))) {
		goto done;
	}
	if (peer->present || peer->config == NULL) {
		result = 0;
		goto done;
	}
	deadline = peer->transport.now_ms(peer->transport.context) + 30000;
	do {
		absent = peer->transport.interfaces(peer->transport.context, &interfaces) == MESH_OK;
		for (i = 0; absent && i < interfaces.count; i++) {
			if (strcmp(interfaces.entries[i].ifname, peer->config->mesh_ifname) == 0) {
				absent = false;
			}
		}
		if (absent && untouched(peer, &interfaces)) {
			mesh_runtime_data_clear(&reply);
			if (peer->transport.invoke(peer->transport.context, "network.interface.mesh11sd_v8_management",
									   "status", request.blob.head, &reply) == 4) {
				if (!peer->config->manage_services ||
					peer->transport.invoke(peer->transport.context, "network.interface.mesh11sd_v8_client",
										   "status", request.blob.head, &reply) == 4) {
					result = 0;
					break;
				}
			}
		}
		if (peer->transport.now_ms(peer->transport.context) >= deadline) {
			break;
		}
		peer->transport.wait_ms(peer->transport.context, 500);
	} while (++attempts < 61);
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}
