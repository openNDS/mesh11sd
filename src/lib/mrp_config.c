/**
 * @file mrp_config.c
 * @brief Read committed LAN, WAN, and dnsmasq evidence for the IPv4 MRP profile.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "mrp.h"
#include <uci.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *value(struct uci_context *context, struct uci_section *section, const char *name)
{
	return uci_lookup_option_string(context, section, name);
}

static bool identifier(const char *text, size_t capacity)
{
	size_t i;
	if (text == NULL || text[0] == '\0' || strlen(text) >= capacity) {
		return false;
	}
	for (i = 0; text[i] != '\0'; i++) {
		if (!isalnum((unsigned char)text[i]) && text[i] != '_') {
			return false;
		}
	}
	return true;
}

static bool device_name(const char *name)
{
	size_t i;
	if (name == NULL || name[0] == '\0' || strlen(name) >= 16 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return false;
	}
	for (i = 0; name[i] != '\0'; i++) {
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.') {
			return false;
		}
	}
	return true;
}

static bool includes(struct uci_context *context, struct uci_section *section,
					 const char *name, const char *needle, bool exclusive)
{
	struct uci_option *option = uci_lookup_option(context, section, name);
	struct uci_element *item;
	bool found = false;
	if (option == NULL) {
		return false;
	}
	if (option->type == UCI_TYPE_STRING) {
		return strcmp(option->v.string, needle) == 0;
	}
	if (option->type != UCI_TYPE_LIST) {
		return false;
	}
	uci_foreach_element(&option->v.list, item) {
		if (strcmp(item->name, needle) == 0) {
			found = true;
		} else if (exclusive) {
			return false;
		}
	}
	return found;
}

static bool enabled(struct uci_context *context, struct uci_section *section, const char *name)
{
	const char *text = value(context, section, name);
	return text != NULL && (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 ||
							strcmp(text, "yes") == 0 || strcmp(text, "on") == 0 || strcmp(text, "enabled") == 0);
}

static int import(int root, struct uci_context *context, const char *name,
				   struct uci_package **package, struct mesh_file_stamp *stamp)
{
	char path[128];
	unsigned char *text = calloc(1, MESH_INPUT_MAX + 1);
	size_t length;
	FILE *input = NULL;
	int result = -1;
	snprintf(path, sizeof(path), "etc/config/%s", name);
	if (text != NULL && mesh_read_bounded_at(root, path, text, MESH_INPUT_MAX, &length, stamp) == 0 &&
		memchr(text, '\0', length) == NULL && (input = fmemopen(text, length, "r")) != NULL &&
		uci_import(context, input, name, package, true) == UCI_OK) {
		result = 0;
	}
	if (input != NULL) {
		fclose(input);
	}
	if (text != NULL) {
		mesh_sensitive_clear(text, MESH_INPUT_MAX + 1);
		free(text);
	}
	return result;
}

static int zones(struct uci_context *context, struct uci_package *package, struct mesh_native_config *config)
{
	struct uci_element *element;
	struct uci_section *section;
	const char *name;
	const char *family;
	unsigned int lan = 0;
	unsigned int wan = 0;
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "zone") != 0) {
			continue;
		}
		name = value(context, section, "enabled");
		if (name != NULL && strcmp(name, "0") == 0) {
			continue;
		}
		if (!includes(context, section, "network", config->network, false) &&
			!includes(context, section, "network", "wan", false)) {
			continue;
		}
		name = value(context, section, "name");
		family = value(context, section, "family");
		if (!identifier(name, 32) || (family != NULL && strcmp(family, "ipv6") == 0) ||
			uci_lookup_option(context, section, "subnet") != NULL) {
			return -1;
		}
		if (includes(context, section, "network", config->network, false)) {
			if (!includes(context, section, "network", config->network, true) ||
				(uci_lookup_option(context, section, "device") != NULL &&
				 !includes(context, section, "device", config->bridge_ifname, true))) {
				return -1;
			}
			memcpy(config->mrp.lan_zone, name, strlen(name) + 1);
			lan++;
		}
		if (includes(context, section, "network", "wan", false)) {
			memcpy(config->mrp.wan_zone, name, strlen(name) + 1);
			wan++;
		}
	}
	return lan == 1 && wan == 1 && strcmp(config->mrp.lan_zone, config->mrp.wan_zone) != 0 ? 0 : -1;
}

static int dns_owner(struct uci_context *context, struct uci_package *package, struct mesh_native_config *config)
{
	struct uci_element *element;
	struct uci_section *section;
	struct uci_section *pool = NULL;
	struct uci_section *owner = NULL;
	const char *interface;
	const char *binding;
	const char *ignore;
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		interface = value(context, section, "interface");
		if (strcmp(section->type, "dhcp") == 0 &&
			(strcmp(section->e.name, config->network) == 0 ||
			 (interface != NULL && strcmp(interface, config->network) == 0))) {
			if (pool != NULL) {
				return -1;
			}
			pool = section;
		}
	}
	if (pool == NULL || enabled(context, pool, "ignore") ||
		(value(context, pool, "dhcpv4") != NULL && strcmp(value(context, pool, "dhcpv4"), "disabled") == 0)) {
		return -1;
	}
	/* Preserve supported bounds/lease overrides. Do not silently drop custom
	 * IPv4 options when netifd's stable LAN intentionally uses proto=none. */
	uci_foreach_element(&pool->options, element) {
		const char *name = element->name;
		if (strcmp(name, "interface") != 0 && strcmp(name, "instance") != 0 &&
			strcmp(name, "start") != 0 && strcmp(name, "limit") != 0 && strcmp(name, "leasetime") != 0 &&
			strcmp(name, "ignore") != 0 && strcmp(name, "dhcpv4") != 0 && strcmp(name, "dhcpv6") != 0 &&
			strcmp(name, "ndp") != 0 && strcmp(name, "ra") != 0 && strncmp(name, "ra_", 3) != 0) {
			return -1;
		}
	}
	binding = value(context, pool, "instance");
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "dnsmasq") != 0 || enabled(context, section, "disabled") ||
			(binding != NULL && strcmp(binding, section->e.name) != 0)) {
			continue;
		}
		if (owner != NULL || !identifier(section->e.name, 64)) {
			return -1;
		}
		owner = section;
	}
	if (owner == NULL || enabled(context, owner, "noping") || enabled(context, owner, "sequential_ip") ||
		(value(context, owner, "port") != NULL && strcmp(value(context, owner, "port"), "53") != 0)) {
		return -1;
	}
	/* dhcp-authoritative is instance-wide. Never change another active pool's
	 * semantics: beta requires this owner to have no unrelated enabled pools. */
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		if (section == pool || strcmp(section->type, "dhcp") != 0) {
			continue;
		}
		ignore = value(context, section, "ignore");
		binding = value(context, section, "instance");
		if ((ignore == NULL || strcmp(ignore, "1") != 0) &&
			(binding == NULL || strcmp(binding, owner->e.name) == 0)) {
			return -1;
		}
	}
	memset(config->dns_instances, 0, sizeof(config->dns_instances));
	memcpy(config->dns_instances[0], owner->e.name, strlen(owner->e.name) + 1);
	config->dns_instance_count = 1;
	config->dns_suppression_needed = true;
	config->manage_services = true;
	return 0;
}

static bool disjoint_addresses(struct blob_attr *addresses, const struct mesh_ipv4 *pool, bool required)
{
	struct blob_attr *item;
	struct blob_attr *address;
	struct blob_attr *bits;
	struct in_addr parsed;
	uint32_t prefix;
	uint32_t common;
	int left;
	bool found = false;
	if (pool->prefix == 0 || pool->prefix > 30) {
		return false;
	}
	if (addresses == NULL) {
		return !required;
	}
	blobmsg_for_each_attr(item, addresses, left) {
		address = mesh_field(item, "address", BLOBMSG_TYPE_STRING);
		bits = mesh_field(item, "mask", BLOBMSG_TYPE_INT32);
		if (address == NULL || bits == NULL || (prefix = blobmsg_get_u32(bits)) == 0 || prefix > 32 ||
			inet_pton(AF_INET, blobmsg_get_string(address), &parsed) != 1) {
			return false;
		}
		common = UINT32_MAX << (32 - (prefix < pool->prefix ? prefix : pool->prefix));
		if ((ntohl(parsed.s_addr) & common) == (pool->address & common)) {
			return false;
		}
		found = true;
	}
	return found || !required;
}

static bool foreign_subnets(struct mesh_runtime_data *reply, const struct mesh_native_config *config)
{
	struct blob_attr *interfaces = mesh_root_field(reply, "interface", BLOBMSG_TYPE_ARRAY);
	struct blob_attr *item;
	struct blob_attr *name;
	int left;
	const char *text;
	if (interfaces == NULL) {
		return false;
	}
	blobmsg_for_each_attr(item, interfaces, left) {
		name = mesh_field(item, "interface", BLOBMSG_TYPE_STRING);
		if (name == NULL) {
			return false;
		}
		text = blobmsg_get_string(name);
		if (strcmp(text, config->network) == 0 || strcmp(text, "mesh11sd_v8_client") == 0 ||
			strcmp(text, "mesh11sd_v8_management") == 0 || strcmp(text, "mesh11sd_v8_bridge_anchor") == 0) {
			continue;
		}
		if (!disjoint_addresses(mesh_field(item, "ipv4-address", BLOBMSG_TYPE_ARRAY), &config->config.portal_ipv4, false)) {
			return false;
		}
	}
	return true;
}

int mesh_mrp_configure(int root, struct mesh_peer *peer, struct mesh_native_config *config,
					   enum mesh_mrp_profile profile)
{
	struct uci_context *context = NULL;
	struct uci_package *firewall = NULL;
	struct uci_package *dhcp = NULL;
	struct mesh_file_stamp stamp;
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *field;
	struct blob_attr *routes;
	struct blob_attr *route;
	struct blob_attr *target;
	struct blob_attr *mask;
	char forwarding[8];
	size_t size;
	size_t i;
	int left;
	int fd;
	bool default_route = false;
	int result = -1;
	mesh_message_init(&request);
	if (config == NULL || peer == NULL) {
		mesh_message_clear(&request);
		return -1;
	}
	memset(&config->mrp, 0, sizeof(config->mrp));
	if (profile != MESH_MRP_IPV4_ONLY) {
		result = -2;
		goto done;
	}
	fd = mesh_open_at_root(root, "tmp/.uci/firewall", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		close(fd);
		goto done;
	}
	if (errno != ENOENT || config->stamp_count > MESH_INPUT_FILES ||
		mesh_read_bounded_at(root, "proc/sys/net/ipv4/ip_forward", forwarding, sizeof(forwarding), &size, NULL) != 0 ||
		!((size == 2 && memcmp(forwarding, "1\n", 2) == 0) || (size == 1 && forwarding[0] == '1')) ||
		mesh_peer_call(peer, "network.interface.wan", "status", &request, &reply) != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
	if (field == NULL || !blobmsg_get_bool(field)) {
		goto done;
	}
	field = mesh_root_field(&reply, "l3_device", BLOBMSG_TYPE_STRING);
	if (field == NULL || !device_name(blobmsg_get_string(field)) ||
		strcmp(blobmsg_get_string(field), config->bridge_ifname) == 0) {
		goto done;
	}
	memcpy(config->mrp.uplink, blobmsg_get_string(field), strlen(blobmsg_get_string(field)) + 1);
	routes = mesh_root_field(&reply, "route", BLOBMSG_TYPE_ARRAY);
	if (routes == NULL) {
		goto done;
	}
	blobmsg_for_each_attr(route, routes, left) {
		target = mesh_field(route, "target", BLOBMSG_TYPE_STRING);
		mask = mesh_field(route, "mask", BLOBMSG_TYPE_INT32);
		default_route = default_route || (target != NULL && mask != NULL && blobmsg_get_u32(mask) == 0 &&
										 strcmp(blobmsg_get_string(target), "0.0.0.0") == 0);
	}
	field = mesh_root_field(&reply, "dns-server", BLOBMSG_TYPE_ARRAY);
	if (!default_route || field == NULL || blobmsg_data_len(field) == 0 ||
		!disjoint_addresses(mesh_root_field(&reply, "ipv4-address", BLOBMSG_TYPE_ARRAY), &config->config.portal_ipv4, true) ||
		mesh_peer_call(peer, "network.interface", "dump", &request, &reply) != 0 || !foreign_subnets(&reply, config) ||
		(context = uci_alloc_context()) == NULL ||
		import(root, context, "firewall", &firewall, &stamp) != 0 || zones(context, firewall, config) != 0 ||
		import(root, context, "dhcp", &dhcp, NULL) != 0 || dns_owner(context, dhcp, config) != 0) {
		goto done;
	}
	/* Repeated configure replaces its own firewall stamp, never grows it. */
	for (i = 0; i < config->stamp_count; i++) {
		if (strcmp(config->stamps[i].relative_path, "etc/config/firewall") == 0) {
			break;
		}
	}
	if (i == MESH_INPUT_FILES) {
		goto done;
	}
	config->stamps[i] = stamp;
	if (i == config->stamp_count) {
		config->stamp_count++;
	}
	config->mrp.profile = profile;
	result = 0;
done:
	if (context != NULL) {
		uci_free_context(context);
	}
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	if (result != 0) {
		memset(&config->mrp, 0, sizeof(config->mrp));
	}
	return result;
}
