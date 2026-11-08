/**
 * @file bridge_observe.c
 * @brief Observe bridge membership, MAC, and STP state from the kernel.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "bridge.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool ap_member(struct mesh_bridge *bridge, const char *name)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *radio;
	struct blob_attr *interfaces;
	struct blob_attr *entry;
	struct blob_attr *field;
	struct blob_attr *config;
	struct blob_attr *network;
	struct blob_attr *item;
	int left;
	int remaining;
	int values;
	bool found = false;
	mesh_message_init(&request);
	if (mesh_peer_call(bridge->peer, "network.wireless", "status", &request, &reply) != 0) {
		goto done;
	}
	blob_for_each_attr(radio, (struct blob_attr *)reply.bytes, left) {
		interfaces = mesh_field(radio, "interfaces", BLOBMSG_TYPE_ARRAY);
		if (interfaces == NULL) {
			continue;
		}
		blobmsg_for_each_attr(entry, interfaces, remaining) {
			field = mesh_field(entry, "ifname", BLOBMSG_TYPE_STRING);
			if (field == NULL || strcmp(blobmsg_get_string(field), name) != 0) {
				continue;
			}
			config = mesh_field(entry, "config", BLOBMSG_TYPE_TABLE);
			field = mesh_field(config, "mode", BLOBMSG_TYPE_STRING);
			network = mesh_field(config, "network", BLOBMSG_TYPE_ARRAY);
			if (field == NULL || strcmp(blobmsg_get_string(field), "ap") != 0 || network == NULL) {
				continue;
			}
			blobmsg_for_each_attr(item, network, values) {
				if (blobmsg_type(item) == BLOBMSG_TYPE_STRING &&
					strcmp(blobmsg_get_string(item), bridge->disk.config.network) == 0) {
					found = true;
				}
			}
		}
	}
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return found;
}

static bool wired(const struct mesh_bridge_config *config, const char *name)
{
	size_t i;
	for (i = 0; i < config->port_count; i++) {
		if (strcmp(config->ports[i], name) == 0) {
			return true;
		}
	}
	return false;
}

static int path_exists(int root, const char *path)
{
	int fd = mesh_open_at_root(root, path, O_PATH | O_CLOEXEC);
	if (fd < 0) {
		return errno == ENOENT ? 0 : -1;
	}
	close(fd);
	return 1;
}

int mesh_bridge_ports_guard(struct mesh_bridge *bridge)
{
	char path[128];
	char target[512];
	char text[64];
	char *end;
	char *base;
	size_t length;
	size_t i;
	ssize_t size;
	unsigned long index;
	for (i = 0; i < bridge->disk.config.port_count; i++) {
		if (bridge->disk.config.wan[0] != '\0' &&
			!mesh_bridge_links_disjoint(bridge->root, bridge->disk.config.ports[i], bridge->disk.config.wan)) {
			return -1;
		}
		snprintf(path, sizeof(path), "sys/class/net/%s/ifindex", bridge->disk.config.ports[i]);
		if (mesh_read_bounded_at(bridge->root, path, text, sizeof(text) - 1, &length, NULL) != 0 || length == 0) {
			return -1;
		}
		text[length] = '\0';
		errno = 0;
		index = strtoul(text, &end, 10);
		if (errno != 0 || index == 0 || (*end != '\0' && strcmp(end, "\n") != 0)) {
			return -1;
		}
		snprintf(path, sizeof(path), "sys/class/net/%s/bridge", bridge->disk.config.ports[i]);
		if (path_exists(bridge->root, path) != 0) {
			return -1;
		}
		snprintf(path, sizeof(path), "sys/class/net/%s/phy80211", bridge->disk.config.ports[i]);
		if (path_exists(bridge->root, path) != 0) {
			return -1;
		}
		snprintf(path, sizeof(path), "sys/class/net/%s/master", bridge->disk.config.ports[i]);
		size = mesh_readlink_at_root(bridge->root, path, target, sizeof(target) - 1);
		if (size < 0 && errno == ENOENT) {
			continue;
		}
		if (size <= 0 || (size_t)size >= sizeof(target) - 1) {
			return -1;
		}
		target[size] = '\0';
		base = strrchr(target, '/');
		if (strcmp(base != NULL ? base + 1 : target, bridge->disk.config.name) != 0) {
			return -1;
		}
	}
	return 0;
}

int mesh_bridge_probe(struct mesh_bridge *bridge, uint8_t mac[6], bool *stp, bool require_ports)
{
	char path[128];
	char text[64];
	size_t length;
	size_t i;
	size_t count = 0;
	struct dirent *entry;
	DIR *directory;
	int descriptor;
	int result = -1;
	bool seen[MESH_BRIDGE_PORTS] = {0};
	snprintf(path, sizeof(path), "sys/class/net/%s/bridge/stp_state", bridge->disk.config.name);
	if (mesh_read_bounded_at(bridge->root, path, text, sizeof(text) - 1, &length, NULL) != 0) {
		if (errno != ENOENT) {
			return -1;
		}
		snprintf(path, sizeof(path), "sys/class/net/%s", bridge->disk.config.name);
		return path_exists(bridge->root, path) == 0 ? 1 : -1;
	}
	if (length < 1 || length > 2 || (text[0] != '0' && text[0] != '1') || (length == 2 && text[1] != '\n')) {
		return -1;
	}
	*stp = text[0] == '1';
	snprintf(path, sizeof(path), "sys/class/net/%s/address", bridge->disk.config.name);
	if (mesh_read_bounded_at(bridge->root, path, text, sizeof(text) - 1, &length, NULL) != 0 || length < 17 || length > 18) {
		return -1;
	}
	text[length] = '\0';
	if (length == 18 && text[17] == '\n') {
		text[17] = '\0';
	}
	if (mesh_mac_parse(text, mac) != 0) {
		return -1;
	}
	snprintf(path, sizeof(path), "sys/class/net/%s/brif", bridge->disk.config.name);
	descriptor = mesh_open_at_root(bridge->root, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0) {
		return -1;
	}
	directory = fdopendir(descriptor);
	if (directory == NULL) {
		close(descriptor);
		return -1;
	}
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		if (++count > MESH_BRIDGE_PORTS + MESH_APS_MAX + 1) {
			goto done;
		}
		if (wired(&bridge->disk.config, entry->d_name)) {
			for (i = 0; i < bridge->disk.config.port_count; i++) {
				seen[i] = seen[i] || strcmp(bridge->disk.config.ports[i], entry->d_name) == 0;
			}
		} else if (!(bridge->peer->present && strcmp(entry->d_name, bridge->disk.config.mesh_ifname) == 0) &&
				   !ap_member(bridge, entry->d_name)) {
			goto done;
		}
		errno = 0;
	}
	if (errno != 0) {
		goto done;
	}
	for (i = 0; require_ports && i < bridge->disk.config.port_count; i++) {
		if (!seen[i]) {
			goto done;
		}
	}
	result = 0;
done:
	closedir(directory);
	return result;
}
