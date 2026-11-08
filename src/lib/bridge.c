/**
 * @file bridge.c
 * @brief Owned bridge publish, verify, and withdrawal lifecycle.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BRIDGE_MAGIC UINT32_C(0x4d384252)

static bool equal(const struct mesh_runtime_data *a, const struct mesh_runtime_data *b)
{
	return a->length == b->length && a->length <= MESH_RUNTIME_BYTES && memcmp(a->bytes, b->bytes, a->length) == 0;
}

int mesh_bridge_payload(const struct mesh_bridge_disk *state, bool pinned, struct mesh_runtime_data *output)
{
	struct mesh_message message;
	void *kind;
	void *device;
	void *ports;
	char mac[18];
	const uint8_t *address = pinned ? state->actual_mac : state->original_present ? state->original_mac : state->config.configured_mac;
	bool have_mac = pinned || state->original_present || state->config.mac_set;
	size_t i;
	int result = -1;
	if (state->config.port_count > MESH_BRIDGE_PORTS || state->config.option_count > MESH_BRIDGE_OPTIONS ||
		(have_mac && !mesh_mac_valid(address))) {
		return -1;
	}
	mesh_message_init(&message);
	kind = blobmsg_open_table(&message.blob, "network-device");
	device = blobmsg_open_table(&message.blob, state->config.name);
	if (kind == NULL || device == NULL || blobmsg_add_string(&message.blob, "type", "bridge") != 0 ||
		blobmsg_add_string(&message.blob, "name", state->config.name) != 0 ||
		blobmsg_add_u8(&message.blob, "stp", false) != 0 ||
		blobmsg_add_u8(&message.blob, "bridge_empty", true) != 0 ||
		blobmsg_add_u8(&message.blob, "vlan_filtering", false) != 0) {
		goto done;
	}
	if (have_mac) {
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2], address[3], address[4], address[5]);
		if (blobmsg_add_string(&message.blob, "macaddr", mac) != 0) {
			goto done;
		}
	}
	ports = blobmsg_open_array(&message.blob, "ports");
	if (ports == NULL) {
		goto done;
	}
	for (i = 0; i < state->config.port_count; i++) {
		if (blobmsg_add_string(&message.blob, NULL, state->config.ports[i]) != 0) {
			goto done;
		}
	}
	blobmsg_close_array(&message.blob, ports);
	for (i = 0; i < state->config.option_count; i++) {
		const struct mesh_bridge_option *option = &state->config.options[i];
		if ((option->boolean ? blobmsg_add_u8(&message.blob, option->name, option->value != 0) :
			 blobmsg_add_u32(&message.blob, option->name, option->value)) != 0) {
			goto done;
		}
	}
	blobmsg_close_table(&message.blob, device);
	blobmsg_close_table(&message.blob, kind);
	kind = blobmsg_open_table(&message.blob, "network-interface");
	device = blobmsg_open_table(&message.blob, MESH_BRIDGE_ANCHOR);
	if (kind == NULL || device == NULL || blobmsg_add_string(&message.blob, "proto", "none") != 0 ||
		blobmsg_add_string(&message.blob, "device", state->config.name) != 0 ||
		blobmsg_add_u8(&message.blob, "auto", true) != 0 || blobmsg_add_u8(&message.blob, "force_link", true) != 0) {
		goto done;
	}
	blobmsg_close_table(&message.blob, device);
	blobmsg_close_table(&message.blob, kind);
	result = mesh_message_copy(&message, output);
done:
	mesh_message_clear(&message);
	return result;
}

static int foreign_owners(struct mesh_bridge *bridge)
{
	static const char *const kinds[] = { "network-device", "network-interface" };
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *service;
	struct blob_attr *instance;
	struct blob_attr *objects;
	struct blob_attr *object;
	struct blob_attr *device;
	int remaining;
	int entries;
	int values;
	size_t i;
	int result = -1;
	for (i = 0; i < 2; i++) {
		mesh_message_init(&request);
		if (blobmsg_add_string(&request.blob, "type", kinds[i]) != 0 ||
			mesh_peer_call(bridge->peer, "service", "get_data", &request, &reply) != 0 || !mesh_reply_valid(&reply)) {
			mesh_message_clear(&request);
			goto done;
		}
		mesh_message_clear(&request);
		blob_for_each_attr(service, (struct blob_attr *)reply.bytes, remaining) {
			if (blobmsg_type(service) != BLOBMSG_TYPE_TABLE) {
				goto done;
			}
			if (strcmp(blobmsg_name(service), MESH_BRIDGE_SERVICE) == 0 ||
				(i == 1 && bridge->peer->present && strcmp(blobmsg_name(service), MESH_PEER_SERVICE) == 0)) {
				continue;
			}
			blobmsg_for_each_attr(instance, service, entries) {
				objects = mesh_field(instance, kinds[i], BLOBMSG_TYPE_TABLE);
				if (objects == NULL) {
					goto done;
				}
				blobmsg_for_each_attr(object, objects, values) {
					if (i == 0 && strcmp(blobmsg_name(object), bridge->disk.config.name) == 0) {
						goto done;
					}
					if (i == 1) {
						device = mesh_field(object, "device", BLOBMSG_TYPE_STRING);
						if (strcmp(blobmsg_name(object), MESH_BRIDGE_ANCHOR) == 0 ||
							strcmp(blobmsg_name(object), bridge->disk.config.network) == 0 ||
							(device != NULL && strcmp(blobmsg_get_string(device), bridge->disk.config.name) == 0)) {
							goto done;
						}
					}
				}
			}
		}
	}
	result = 0;
done:
	mesh_runtime_data_clear(&reply);
	return result;
}

static int anchor_status(struct mesh_bridge *bridge, bool expected);

static int read_service(struct mesh_bridge *bridge, struct mesh_runtime_data *output)
{
	struct mesh_message request;
	struct mesh_message normalized;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *service;
	struct blob_attr *data;
	struct blob_attr *field;
	struct blob_attr *item;
	int remaining;
	size_t count = 0;
	int result = -1;
	mesh_message_init(&request);
	mesh_message_init(&normalized);
	if (blobmsg_add_string(&request.blob, "name", MESH_BRIDGE_SERVICE) != 0 ||
		mesh_peer_call(bridge->peer, "service", "list", &request, &reply) != 0 || !mesh_reply_valid(&reply)) {
		goto done;
	}
	service = mesh_root_field(&reply, MESH_BRIDGE_SERVICE, BLOBMSG_TYPE_TABLE);
	if (service == NULL) {
		if (blob_len((struct blob_attr *)reply.bytes) == 0) {
			mesh_runtime_data_clear(output);
			bridge->present = false;
			result = 0;
		}
		goto done;
	}
	field = mesh_field(service, "autostart", BLOBMSG_TYPE_BOOL);
	if (field == NULL || blobmsg_get_bool(field)) {
		goto done;
	}
	field = mesh_field(service, "instances", -1);
	if (field != NULL && (blobmsg_type(field) != BLOBMSG_TYPE_TABLE || blobmsg_data_len(field) != 0)) {
		goto done;
	}
	data = mesh_field(service, "data", BLOBMSG_TYPE_TABLE);
	field = mesh_field(data, MESH_PEER_OWNER, BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), bridge->peer->owner) != 0) {
		goto done;
	}
	blobmsg_for_each_attr(item, data, remaining) {
		count++;
	}
	if (count != 3 || (field = mesh_field(data, "network-device", BLOBMSG_TYPE_TABLE)) == NULL ||
		blobmsg_add_blob(&normalized.blob, field) != 0 ||
		(field = mesh_field(data, "network-interface", BLOBMSG_TYPE_TABLE)) == NULL ||
		blobmsg_add_blob(&normalized.blob, field) != 0 || mesh_message_copy(&normalized, output) != 0) {
		goto done;
	}
	bridge->present = true;
	result = 0;
done:
	mesh_message_clear(&request);
	mesh_message_clear(&normalized);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int replace(struct mesh_bridge *bridge, const struct mesh_runtime_data *wanted)
{
	struct mesh_runtime_data current = {0};
	struct mesh_runtime_data after = {0};
	struct mesh_runtime_data reply = {0};
	struct mesh_message request;
	struct blob_attr *field;
	void *data;
	int remaining;
	int result = -1;
	const char *method;
	mesh_message_init(&request);
	if (read_service(bridge, &current) != 0 || !equal(&current, &bridge->installed) ||
		blobmsg_add_string(&request.blob, "name", MESH_BRIDGE_SERVICE) != 0) {
		goto done;
	}
	method = wanted->length == 0 ? "delete" : bridge->present ? "set_data" : "set";
	if (wanted->length != 0) {
		if (!bridge->present && blobmsg_add_u8(&request.blob, "autostart", false) != 0) {
			goto done;
		}
		data = blobmsg_open_table(&request.blob, "data");
		if (data == NULL || blobmsg_add_string(&request.blob, MESH_PEER_OWNER, bridge->peer->owner) != 0) {
			goto done;
		}
		blob_for_each_attr(field, (struct blob_attr *)wanted->bytes, remaining) {
			if (blobmsg_add_blob(&request.blob, field) != 0) {
				goto done;
			}
		}
		blobmsg_close_table(&request.blob, data);
	}
	bridge->peer->transport.invoke(bridge->peer->transport.context, "service", method, request.blob.head, &reply);
	if (read_service(bridge, &after) == 0 && equal(&after, wanted)) {
		bridge->installed = *wanted;
		result = 0;
	}
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&current);
	mesh_runtime_data_clear(&after);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int reload(struct mesh_bridge *bridge)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	int result;
	mesh_message_init(&request);
	result = mesh_peer_call(bridge->peer, "network", "reload", &request, &reply);
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int journal(struct mesh_bridge *bridge)
{
	struct mesh_bridge_disk check;
	size_t length;
	int directory = bridge->peer->directory;
	int fd;
	int result;
	if (bridge->journal_exists && mesh_stamp_unchanged(directory, &bridge->journal_stamp) != 0) {
		return -1;
	}
	fd = openat(directory, "bridge.pending", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	result = write(fd, &bridge->disk, sizeof(bridge->disk)) == sizeof(bridge->disk) && fsync(fd) == 0 ? 0 : -1;
	if (close(fd) != 0) {
		result = -1;
	}
	if (result != 0) {
		unlinkat(directory, "bridge.pending", 0);
		return -1;
	}
	if ((bridge->journal_exists ? renameat(directory, "bridge.pending", directory, "bridge.meta") :
		 (int)syscall(SYS_renameat2, directory, "bridge.pending", directory, "bridge.meta", RENAME_NOREPLACE)) != 0) {
		return -1;
	}
	bridge->journal_exists = true;
	return fsync(directory) == 0 && mesh_read_bounded_at(directory, "bridge.meta", &check, sizeof(check),
													   &length, &bridge->journal_stamp) == 0 &&
		   length == sizeof(check) && memcmp(&check, &bridge->disk, sizeof(check)) == 0 ? 0 : -1;
}

static bool terminated(const char *text, size_t size)
{
	return memchr(text, '\0', size) != NULL;
}

static bool valid_disk(struct mesh_bridge *bridge)
{
	struct mesh_bridge_disk *disk = &bridge->disk;
	size_t i;
	if (disk->magic != BRIDGE_MAGIC || disk->version != 1 || disk->size != sizeof(*disk) ||
		disk->phase < 1 || disk->phase > 3 || !terminated(disk->owner, sizeof(disk->owner)) ||
		strcmp(disk->owner, bridge->peer->owner) != 0 || disk->original_present > 1 || disk->pinned > 1 ||
		!terminated(disk->config.name, sizeof(disk->config.name)) ||
		!terminated(disk->config.network, sizeof(disk->config.network)) ||
		!terminated(disk->config.mesh_ifname, sizeof(disk->config.mesh_ifname)) ||
		!terminated(disk->config.wan, sizeof(disk->config.wan)) || disk->config.port_count > MESH_BRIDGE_PORTS ||
		disk->config.option_count > MESH_BRIDGE_OPTIONS || disk->config.stp > 1 || disk->config.mac_set > 1 ||
		(disk->original_present && !mesh_mac_valid(disk->original_mac)) ||
		(disk->pinned && !mesh_mac_valid(disk->actual_mac))) {
		return false;
	}
	for (i = 0; i < disk->config.port_count; i++) {
		if (!terminated(disk->config.ports[i], sizeof(disk->config.ports[i]))) {
			return false;
		}
	}
	for (i = 0; i < disk->config.option_count; i++) {
		if (!terminated(disk->config.options[i].name, sizeof(disk->config.options[i].name))) {
			return false;
		}
	}
	return true;
}

int mesh_bridge_open(struct mesh_bridge *bridge, struct mesh_peer *peer, int root,
					 const struct mesh_native_config *config)
{
	struct mesh_bridge_config committed;
	struct mesh_runtime_data current = {0};
	struct mesh_runtime_data first = {0};
	struct mesh_runtime_data final = {0};
	struct stat metadata;
	size_t length;
	int probe;
	bool stp;
	int result = -1;
	memset(bridge, 0, sizeof(*bridge));
	bridge->peer = peer;
	bridge->root = root;
	if (peer == NULL || peer->session_lock < 0 || !peer->initialized || config == NULL ||
		mesh_bridge_config_read(root, config, &committed, &bridge->network_stamp) != 0 ||
		fstatat(peer->directory, "bridge.pending", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
		read_service(bridge, &current) != 0) {
		goto done;
	}
	if (fstatat(peer->directory, "bridge.meta", &metadata, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_uid != geteuid() ||
			(metadata.st_mode & 0777) != 0600 ||
			mesh_read_bounded_at(peer->directory, "bridge.meta", &bridge->disk, sizeof(bridge->disk),
								 &length, &bridge->journal_stamp) != 0 || length != sizeof(bridge->disk) ||
			!valid_disk(bridge) || memcmp(&bridge->disk.config, &committed, sizeof(committed)) != 0 ||
			mesh_bridge_payload(&bridge->disk, false, &first) != 0 ||
			(bridge->disk.pinned && mesh_bridge_payload(&bridge->disk, true, &final) != 0) ||
			(current.length != 0 && !equal(&current, &first) && !equal(&current, &final))) {
			goto done;
		}
		bridge->journal_exists = true;
		bridge->recovering = true;
	} else {
		if (errno != ENOENT || current.length != 0) {
			goto done;
		}
		bridge->disk.magic = BRIDGE_MAGIC;
		bridge->disk.version = 1;
		bridge->disk.size = sizeof(bridge->disk);
		bridge->disk.phase = 1;
		bridge->disk.config = committed;
		memcpy(bridge->disk.owner, peer->owner, sizeof(bridge->disk.owner));
		probe = mesh_bridge_probe(bridge, bridge->disk.original_mac, &stp, false);
		if (probe < 0 || (probe == 0 && (stp != (bool)committed.stp ||
			(committed.mac_set && memcmp(bridge->disk.original_mac, committed.configured_mac, 6) != 0)))) {
			goto done;
		}
		bridge->disk.original_present = probe == 0;
	}
	if ((!bridge->recovering && peer->present) || mesh_bridge_ports_guard(bridge) != 0 || foreign_owners(bridge) != 0 ||
		(!bridge->recovering && anchor_status(bridge, false) != 0)) {
		goto done;
	}
	bridge->installed = current;
	result = 0;
done:
	mesh_runtime_data_clear(&current);
	mesh_runtime_data_clear(&first);
	mesh_runtime_data_clear(&final);
	return result;
}

static int anchor_status(struct mesh_bridge *bridge, bool expected)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *field;
	int status;
	int result = -1;
	mesh_message_init(&request);
	status = bridge->peer->transport.invoke(bridge->peer->transport.context, "network.interface." MESH_BRIDGE_ANCHOR,
											"status", request.blob.head, &reply);
	if (!expected) {
		result = status == 4 ? 0 : -1;
	} else if (status == 0 && mesh_reply_valid(&reply)) {
		field = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
		if (field == NULL || !blobmsg_get_bool(field)) {
			goto done;
		}
		field = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
		if (field != NULL && strcmp(blobmsg_get_string(field), bridge->disk.config.name) == 0) {
			result = 0;
		}
	}
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_bridge_verify(struct mesh_bridge *bridge)
{
	struct mesh_runtime_data current = {0};
	uint8_t mac[6];
	bool stp;
	int result = -1;
	if (bridge->disk.pinned && mesh_stamp_unchanged(bridge->root, &bridge->network_stamp) == 0 &&
		read_service(bridge, &current) == 0 && equal(&current, &bridge->installed) && current.length != 0 &&
		mesh_bridge_probe(bridge, mac, &stp, true) == 0 && !stp &&
		memcmp(mac, bridge->disk.actual_mac, 6) == 0 && mesh_bridge_ports_guard(bridge) == 0 && foreign_owners(bridge) == 0 &&
		anchor_status(bridge, true) == 0) {
		result = 0;
	}
	mesh_runtime_data_clear(&current);
	return result;
}

int mesh_bridge_apply(struct mesh_bridge *bridge)
{
	struct mesh_runtime_data wanted = {0};
	uint8_t mac[6];
	bool stp;
	unsigned int attempt;
	int result = -1;
	if (bridge->disk.phase == 3 || mesh_stamp_unchanged(bridge->root, &bridge->network_stamp) != 0 ||
		mesh_bridge_ports_guard(bridge) != 0 || foreign_owners(bridge) != 0 ||
		(bridge->peer->cancelled != NULL && *bridge->peer->cancelled)) {
		goto done;
	}
	if (!bridge->present) {
		int observed = mesh_bridge_probe(bridge, mac, &stp, false);
		if (bridge->disk.phase == 2 ||
			(bridge->disk.original_present && (observed != 0 || stp != (bool)bridge->disk.config.stp ||
			 memcmp(mac, bridge->disk.original_mac, 6) != 0)) ||
			(!bridge->disk.original_present && observed != 1)) {
			goto done;
		}
	}
	if (bridge->present && bridge->disk.pinned) {
		struct mesh_runtime_data first = {0};
		struct mesh_runtime_data final = {0};
		bool acceptable = mesh_bridge_probe(bridge, mac, &stp, true) == 0 && !stp &&
						  memcmp(mac, bridge->disk.actual_mac, 6) == 0 &&
						  mesh_bridge_payload(&bridge->disk, false, &first) == 0 &&
						  mesh_bridge_payload(&bridge->disk, true, &final) == 0;
		if (acceptable && equal(&bridge->installed, &first) && !equal(&first, &final)) {
			acceptable = replace(bridge, &final) == 0 && reload(bridge) == 0;
		}
		mesh_runtime_data_clear(&first);
		mesh_runtime_data_clear(&final);
		result = acceptable ? mesh_bridge_verify(bridge) : -1;
		goto done;
	}
	if (!bridge->journal_exists && journal(bridge) != 0) {
		goto done;
	}
	if (mesh_bridge_payload(&bridge->disk, false, &wanted) != 0 ||
		replace(bridge, &wanted) != 0 || reload(bridge) != 0) {
		goto done;
	}
	for (attempt = 0; attempt < 120; attempt++) {
		if (bridge->peer->cancelled != NULL && *bridge->peer->cancelled) {
			goto done;
		}
		if (mesh_bridge_probe(bridge, mac, &stp, true) == 0 && !stp && anchor_status(bridge, true) == 0) {
			break;
		}
		bridge->peer->transport.wait_ms(bridge->peer->transport.context, 250);
	}
	if (attempt == 120 || mesh_stamp_unchanged(bridge->root, &bridge->network_stamp) != 0 ||
		(bridge->disk.original_present && memcmp(mac, bridge->disk.original_mac, 6) != 0) ||
		(bridge->disk.config.mac_set && memcmp(mac, bridge->disk.config.configured_mac, 6) != 0) ||
		(bridge->disk.pinned && memcmp(mac, bridge->disk.actual_mac, 6) != 0)) {
		goto done;
	}
	memcpy(bridge->disk.actual_mac, mac, 6);
	bridge->disk.pinned = 1;
	bridge->disk.phase = 2;
	if (journal(bridge) != 0 || mesh_bridge_payload(&bridge->disk, true, &wanted) != 0 ||
		replace(bridge, &wanted) != 0 || reload(bridge) != 0) {
		goto done;
	}
	for (attempt = 0; attempt < 120; attempt++) {
		if (bridge->peer->cancelled != NULL && *bridge->peer->cancelled) {
			goto done;
		}
		if (mesh_bridge_verify(bridge) == 0) {
			result = 0;
			break;
		}
		bridge->peer->transport.wait_ms(bridge->peer->transport.context, 250);
	}
done:
	mesh_runtime_data_clear(&wanted);
	return result;
}

int mesh_bridge_stop(struct mesh_bridge *bridge)
{
	struct mesh_runtime_data peer = {0};
	struct mesh_runtime_data empty = {0};
	uint8_t mac[6];
	bool stp;
	unsigned int attempt;
	int probe;
	int result = -1;
	if (!bridge->journal_exists) {
		return bridge->installed.length == 0 ? 0 : -1;
	}
	/* Do not restore STP or remove a bridge while a peer projection could still
	 * attach mesh. Scoped role cleanup must have completed first as well. */
	if (bridge->peer->ops.read(bridge->peer, &peer) != 0 || peer.length != 0) {
		goto done;
	}
	{
		struct stat metadata;
		if (fstatat(bridge->peer->directory, "services.meta", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
			goto done;
		}
	}
	{
		struct mesh_interface_list interfaces;
		size_t i;
		if (bridge->peer->transport.interfaces(bridge->peer->transport.context, &interfaces) != MESH_OK) {
			goto done;
		}
		for (i = 0; i < interfaces.count; i++) {
			if (strcmp(interfaces.entries[i].ifname, bridge->disk.config.mesh_ifname) == 0) {
				goto done;
			}
		}
		if (foreign_owners(bridge) != 0) {
			goto done;
		}
		if (bridge->installed.length != 0 && bridge->disk.pinned &&
			(mesh_bridge_probe(bridge, mac, &stp, true) != 0 || stp ||
			 memcmp(mac, bridge->disk.actual_mac, 6) != 0 || mesh_bridge_ports_guard(bridge) != 0)) {
			goto done;
		}
	}
	bridge->released_changed_inputs = mesh_stamp_unchanged(bridge->root, &bridge->network_stamp) != 0;
	bridge->disk.phase = 3;
	if (journal(bridge) != 0 || (bridge->installed.length != 0 && replace(bridge, &empty) != 0) || reload(bridge) != 0) {
		goto done;
	}
	for (attempt = 0; attempt < 120; attempt++) {
		probe = mesh_bridge_probe(bridge, mac, &stp, false);
		if (anchor_status(bridge, false) == 0 && (bridge->released_changed_inputs ||
			(!bridge->disk.original_present && probe == 1) ||
			(bridge->disk.original_present && probe == 0 && stp == (bool)bridge->disk.config.stp &&
			 memcmp(mac, bridge->disk.original_mac, 6) == 0))) {
			if (mesh_stamp_unchanged(bridge->peer->directory, &bridge->journal_stamp) == 0 &&
				unlinkat(bridge->peer->directory, "bridge.meta", 0) == 0 && fsync(bridge->peer->directory) == 0) {
				bridge->journal_exists = false;
				result = 0;
			}
			break;
		}
		bridge->peer->transport.wait_ms(bridge->peer->transport.context, 250);
	}
done:
	mesh_runtime_data_clear(&peer);
	return result;
}

void mesh_bridge_clear(struct mesh_bridge *bridge)
{
	mesh_sensitive_clear(bridge, sizeof(*bridge));
}
