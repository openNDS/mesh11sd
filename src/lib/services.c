/**
 * @file services.c
 * @brief Hand off stock LAN services to the owned peer or MRP role.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "services.h"
#include "mrp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libubus.h>

const char *const mesh_service_modes[MESH_SERVICE_MODES] = { "ra", "dhcpv4", "dhcpv6", "ndp" };
static const char *const mode_names[] = { NULL, "disabled", "server", "relay", "hybrid" };
#define SERVICE_OWNER "mesh11sd-v8-services"

static int call(struct mesh_services *services, const char *object, const char *method,
				struct mesh_message *request, struct mesh_runtime_data *reply)
{
	return mesh_peer_call(services->peer, object, method, request, reply);
}

static int status(struct mesh_services *services, struct mesh_runtime_data *reply)
{
	struct mesh_message request;
	char object[96];
	int result;
	snprintf(object, sizeof(object), "network.interface.%s", services->disk.network);
	mesh_message_init(&request);
	result = call(services, object, "status", &request, reply);
	mesh_message_clear(&request);
	return result;
}

static int operation(struct mesh_services *services, const char *method)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	char object[96];
	int result;
	snprintf(object, sizeof(object), "network.interface.%s", services->disk.network);
	mesh_message_init(&request);
	result = call(services, object, method, &request, &reply);
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static bool known_data(struct blob_attr *data, bool allow_owner)
{
	struct blob_attr *field;
	size_t i;
	int remaining;
	bool known;
	blobmsg_for_each_attr(field, data, remaining) {
		known = strcmp(blobmsg_name(field), "zone") == 0 ||
				(allow_owner && strcmp(blobmsg_name(field), SERVICE_OWNER) == 0);
		for (i = 0; i < MESH_SERVICE_MODES; i++) {
			known = known || strcmp(blobmsg_name(field), mesh_service_modes[i]) == 0;
		}
		if (!known) {
			return false;
		}
	}
	return true;
}

static int mode_snapshot(struct mesh_services *services, struct mesh_runtime_data *reply)
{
	struct blob_attr *data = mesh_root_field(reply, "data", BLOBMSG_TYPE_TABLE);
	struct blob_attr *field;
	size_t i;
	size_t j;
	bool found;
	if (data == NULL || !known_data(data, false) || mesh_field(data, SERVICE_OWNER, -1) != NULL) {
		return -1;
	}
	for (i = 0; i < MESH_SERVICE_MODES; i++) {
		field = mesh_field(data, mesh_service_modes[i], -1);
		if (field == NULL) {
			services->disk.modes[i] = 0;
			continue;
		}
		if (blobmsg_type(field) != BLOBMSG_TYPE_STRING) {
			return -1;
		}
		found = false;
		for (j = 1; j < sizeof(mode_names) / sizeof(mode_names[0]); j++) {
			if (strcmp(blobmsg_get_string(field), mode_names[j]) == 0) {
				services->disk.modes[i] = (uint8_t)j;
				found = true;
				break;
			}
		}
		if (!found) {
			return -1;
		}
	}
	return 0;
}

static bool owned_modes(struct mesh_services *services, struct mesh_runtime_data *reply)
{
	struct blob_attr *data = mesh_root_field(reply, "data", BLOBMSG_TYPE_TABLE);
	struct blob_attr *field = mesh_field(data, SERVICE_OWNER, BLOBMSG_TYPE_STRING);
	size_t i;
	if (field == NULL || !known_data(data, true) || strcmp(blobmsg_get_string(field), services->peer->owner) != 0) {
		return false;
	}
	for (i = 0; i < MESH_SERVICE_MODES; i++) {
		field = mesh_field(data, mesh_service_modes[i], BLOBMSG_TYPE_STRING);
		if (field == NULL || strcmp(blobmsg_get_string(field), "disabled") != 0) {
			return false;
		}
	}
	return true;
}

static int modes_set(struct mesh_services *services, bool disable)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	char object[96];
	size_t i;
	int result = -1;

	mesh_message_init(&request);
	for (i = 0; i < MESH_SERVICE_MODES; i++) {
		if (disable || services->disk.modes[i] != 0) {
			if (blobmsg_add_string(&request.blob, mesh_service_modes[i],
								  disable ? "disabled" : mode_names[services->disk.modes[i]]) != 0) {
				goto done;
			}
		}
	}
	if (disable && blobmsg_add_string(&request.blob, SERVICE_OWNER, services->peer->owner) != 0) {
		goto done;
	}
	snprintf(object, sizeof(object), "network.interface.%s", services->disk.network);
	result = call(services, object, "set_data", &request, &reply);
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int servers_quiet(struct mesh_services *services)
{
	static const char *const methods[] = { "ipv6ra", "ipv6leases", "ipv4leases" };
	static const char *const tables[] = { "interfaces", "device", "device" };
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *interfaces;
	size_t i;
	int result = -1;
	int method_result;
	if (mesh_message_init(&request) != 0) {
		return -1;
	}
	for (i = 0; i < 3; i++) {
		method_result = call(services, "dhcp", methods[i], &request, &reply);
		/* Exact odhcpd source compiles ipv4leases out ONLY without DHCPV4_SUPPORT.
		 * Do not reinterpret timeout/object errors as proof of no IPv4 server. */
		if (i == 2 && method_result == UBUS_STATUS_METHOD_NOT_FOUND &&
			services->peer->config != NULL && !services->peer->config->odhcpd_maindhcp) {
			continue;
		}
		if (method_result != 0) {
			goto done;
		}
		interfaces = mesh_root_field(&reply, tables[i], BLOBMSG_TYPE_TABLE);
		/* An empty per-device array still means MODE_SERVER; presence, not
		 * lease count, is what odhcpd's actual ubus response proves. */
		if (interfaces == NULL || mesh_field(interfaces, services->disk.bridge, -1) != NULL) {
			goto done;
		}
	}
	result = 0;
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int wait_state(struct mesh_services *services, bool expected_up)
{
	struct mesh_runtime_data reply = {0};
	struct blob_attr *up;
	struct blob_attr *pending;
	unsigned int attempt;
	int result = -1;
	for (attempt = 0; attempt < 120; attempt++) {
		if (status(services, &reply) == 0) {
			up = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
			pending = mesh_root_field(&reply, "pending", BLOBMSG_TYPE_BOOL);
			if (up != NULL && pending != NULL && blobmsg_get_bool(up) == expected_up && !blobmsg_get_bool(pending)) {
				result = 0;
				break;
			}
		}
		services->peer->transport.wait_ms(services->peer->transport.context, 250);
	}
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_services_open(struct mesh_peer *peer, int root, struct mesh_native_config *config)
{
	struct mesh_services *services;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *up;
	struct blob_attr *pending;
	int loaded;
	int result = -1;

	if (peer == NULL || peer->session_lock < 0 || config == NULL || peer->services != NULL) {
		return -1;
	}
	services = calloc(1, sizeof(*services));
	if (services == NULL) {
		return -1;
	}
	services->peer = peer;
	services->root = root;
	loaded = mesh_services_load(services);
	if (loaded < 0) {
		goto done;
	}
	if (loaded != 0) {
		if ((services->disk.portal_pool.address != 0 && config->mrp.profile != MESH_MRP_IPV4_ONLY) ||
			strcmp(services->disk.network, config->network) != 0 ||
			strcmp(services->disk.bridge, config->bridge_ifname) != 0) {
			goto done;
		}
	} else {
		services->disk.magic = MESH_SERVICE_MAGIC;
		services->disk.version = 2;
		services->disk.size = sizeof(services->disk);
		memcpy(services->disk.owner, peer->owner, sizeof(services->disk.owner));
		memcpy(services->disk.network, config->network, sizeof(services->disk.network));
		memcpy(services->disk.bridge, config->bridge_ifname, sizeof(services->disk.bridge));
		if (status(services, &reply) != 0 || mode_snapshot(services, &reply) != 0) {
			goto done;
		}
		up = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
		pending = mesh_root_field(&reply, "pending", BLOBMSG_TYPE_BOOL);
		if (up == NULL || pending == NULL || blobmsg_get_bool(pending)) {
			goto done;
		}
		services->disk.was_up = blobmsg_get_bool(up);
	}
	if ((services->disk.dns_state != 0 &&
		 memcmp(&services->disk.portal_pool, &config->config.portal_ipv4, sizeof(services->disk.portal_pool)) != 0) ||
		mesh_services_dns_discover(services, config) != 0) {
		goto done;
	}
	peer->services = services;
	config->manage_services = true;
	result = 0;
done:
	mesh_runtime_data_clear(&reply);
	if (result != 0) {
		mesh_sensitive_clear(services, sizeof(*services));
		free(services);
	}
	return result;
}

void mesh_services_close(struct mesh_peer *peer)
{
	if (peer != NULL && peer->services != NULL) {
		mesh_sensitive_clear(peer->services, sizeof(*peer->services));
		free(peer->services);
		peer->services = NULL;
	}
}

static int source_guard(struct mesh_services *services)
{
	size_t i;
	const struct mesh_native_config *config = services->peer->config;
	if (config == NULL || config->stamp_count == 0) {
		return -1;
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(services->root, &config->stamps[i]) != 0) {
			return -1;
		}
	}
	return 0;
}

static int initial_modes_guard(struct mesh_services *services)
{
	struct mesh_runtime_data reply = {0};
	struct blob_attr *data;
	struct blob_attr *field;
	size_t i;
	bool matches = status(services, &reply) == 0;
	data = mesh_root_field(&reply, "data", BLOBMSG_TYPE_TABLE);
	matches = matches && data != NULL && known_data(data, false);
	for (i = 0; matches && i < MESH_SERVICE_MODES; i++) {
		field = mesh_field(data, mesh_service_modes[i], -1);
		if (services->disk.modes[i] == 0) {
			matches = field == NULL;
		} else {
			matches = field != NULL && blobmsg_type(field) == BLOBMSG_TYPE_STRING &&
					  strcmp(blobmsg_get_string(field), mode_names[services->disk.modes[i]]) == 0;
		}
	}
	mesh_runtime_data_clear(&reply);
	return matches ? 0 : -1;
}


int mesh_services_prepare(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	if (services == NULL || source_guard(services) != 0) {
		return -1;
	}
	if (services->disk.phase == 0 && initial_modes_guard(services) != 0) {
		return -1;
	}
	if (!services->journal_exists && mesh_services_journal(services) != 0) {
		return -1;
	}
	if (services->disk.phase != 0) {
		return mesh_services_dns_verify(services);
	}
	if (mesh_services_dns_apply(services, true) != 0) {
		return -1;
	}
	services->disk.phase = 1;
	if (mesh_services_journal(services) != 0) {
		return -1;
	}
	return initial_modes_guard(services);
}

int mesh_services_activate(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	struct mesh_runtime_data reply = {0};
	unsigned int attempt;
	int result = -1;
	if (services == NULL || source_guard(services) != 0 || wait_state(services, true) != 0 ||
		status(services, &reply) != 0) {
		goto done;
	}
	{
		struct blob_attr *data = mesh_root_field(&reply, "data", BLOBMSG_TYPE_TABLE);
		struct blob_attr *field;
		size_t i;
		if (data == NULL || !known_data(data, false)) {
			goto done;
		}
		for (i = 0; i < MESH_SERVICE_MODES; i++) {
			field = mesh_field(data, mesh_service_modes[i], -1);
			if (field != NULL && (services->disk.modes[i] == 0 || blobmsg_type(field) != BLOBMSG_TYPE_STRING ||
				strcmp(blobmsg_get_string(field), mode_names[services->disk.modes[i]]) != 0)) {
				goto done;
			}
		}
	}
	if (modes_set(services, true) != 0) {
		goto done;
	}
	/* The held mesh is still disabled. Stable proto=none LAN is UP, so this
	 * real set_data operation generates odhcpd's interface-update notification.
	 * Only after observed RA suppression do we start the ONE DHCP alias. */
	for (attempt = 0; attempt < 120; attempt++) {
		if (status(services, &reply) == 0 && owned_modes(services, &reply) && servers_quiet(services) == 0) {
			result = 0;
			break;
		}
		peer->transport.wait_ms(peer->transport.context, 250);
	}
	if (result == 0) {
		struct mesh_message request;
		struct mesh_runtime_data client_reply = {0};
		mesh_message_init(&request);
		result = call(services, "network.interface.mesh11sd_v8_client", "up", &request, &client_reply);
		mesh_message_clear(&request);
		mesh_runtime_data_clear(&client_reply);
	}
	if (result == 0) {
		services->disk.phase = 2;
		result = mesh_services_journal(services);
	} else {
		result = -1;
	}
done:
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_services_verify(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	struct mesh_runtime_data reply = {0};
	int result = 0;
	if (services == NULL) {
		return 0;
	}
	if (source_guard(services) != 0 || mesh_services_dns_verify(services) != 0 || status(services, &reply) != 0 ||
		!owned_modes(services, &reply) || servers_quiet(services) != 0) {
		result = -1;
	}
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_services_down(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	struct mesh_runtime_data reply = {0};
	int result = -1;
	if (services == NULL || status(services, &reply) != 0) {
		goto done;
	}
	{
		struct blob_attr *data = mesh_root_field(&reply, "data", BLOBMSG_TYPE_TABLE);
		struct blob_attr *owner = mesh_field(data, SERVICE_OWNER, -1);
		size_t i;
		if (data == NULL || !known_data(data, true) || (owner != NULL && !owned_modes(services, &reply)) ||
			(services->disk.phase == 2 && owner == NULL)) {
			goto done;
		}
		if (services->disk.phase < 2 && owner == NULL) {
			for (i = 0; i < MESH_SERVICE_MODES; i++) {
				struct blob_attr *field = mesh_field(data, mesh_service_modes[i], -1);
				if (field != NULL && (services->disk.modes[i] == 0 || blobmsg_type(field) != BLOBMSG_TYPE_STRING ||
					strcmp(blobmsg_get_string(field), mode_names[services->disk.modes[i]]) != 0)) {
					goto done;
				}
			}
		}
		if (services->disk.phase == 3 && owner == NULL) {
			for (i = 0; i < MESH_SERVICE_MODES; i++) {
				if (mesh_field(data, mesh_service_modes[i], -1) != NULL) {
					goto done;
				}
			}
		}
		services->disk.phase = 3;
		if (mesh_services_journal(services) != 0) {
			goto done;
		}
	}
	/* Only after held-mesh verification: flush our data via the REAL scoped
	 * down path. set_data(null) does NOT remove keys in this netifd version. */
	if (operation(services, "down") == 0 && wait_state(services, false) == 0) {
		result = 0;
	}
done:
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_services_restore(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *data;
	struct blob_attr *field;
	size_t i;
	unsigned int attempt;
	bool matches;
	int result = -1;

	if (services == NULL) {
		return 0;
	}
	if (!services->journal_exists) {
		return 0;
	}
	if (services->disk.dns_state != 0 && mesh_mrp_dns_profile(services, false) != 0) {
		goto done;
	}
	/* Committed reload must have flushed owned data. Never overwrite a foreign
	 * writer's replacement of one of these four scoped mode keys. */
	if (status(services, &reply) != 0 || (data = mesh_root_field(&reply, "data", BLOBMSG_TYPE_TABLE)) == NULL ||
		mesh_field(data, SERVICE_OWNER, -1) != NULL || (!services->disk.was_up && !known_data(data, false))) {
		goto done;
	}
	for (i = 0; i < MESH_SERVICE_MODES; i++) {
		field = mesh_field(data, mesh_service_modes[i], -1);
		if (field != NULL && (services->disk.modes[i] == 0 || blobmsg_type(field) != BLOBMSG_TYPE_STRING ||
			strcmp(blobmsg_get_string(field), mode_names[services->disk.modes[i]]) != 0)) {
			goto done;
		}
	}
	/* Scoped down explicitly clears netifd autostart. A committed reload with
	 * unchanged auto does NOT restore it, including after crash recovery. */
	if (operation(services, services->disk.was_up ? "up" : "down") != 0 ||
		wait_state(services, services->disk.was_up) != 0 ||
		modes_set(services, false) != 0 ||
		mesh_services_dns_apply(services, false) != 0) {
		goto done;
	}
	for (attempt = 0; attempt < 120; attempt++) {
		matches = status(services, &reply) == 0;
		field = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
		matches = matches && field != NULL && blobmsg_get_bool(field) == services->disk.was_up;
		field = mesh_root_field(&reply, "pending", BLOBMSG_TYPE_BOOL);
		matches = matches && field != NULL && !blobmsg_get_bool(field);
		data = mesh_root_field(&reply, "data", BLOBMSG_TYPE_TABLE);
		matches = matches && data != NULL && mesh_field(data, SERVICE_OWNER, -1) == NULL;
		for (i = 0; matches && i < MESH_SERVICE_MODES; i++) {
			field = mesh_field(data, mesh_service_modes[i], -1);
			if (services->disk.modes[i] == 0) {
				matches = field == NULL;
			} else {
				matches = field != NULL && blobmsg_type(field) == BLOBMSG_TYPE_STRING &&
						  strcmp(blobmsg_get_string(field), mode_names[services->disk.modes[i]]) == 0;
			}
		}
		if (matches) {
			result = mesh_services_remove_journal(services);
			break;
		}
		peer->transport.wait_ms(peer->transport.context, 250);
	}
done:
	mesh_runtime_data_clear(&reply);
	return result;
}
