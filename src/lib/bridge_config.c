/**
 * @file bridge_config.c
 * @brief Read the committed LAN bridge, wired ports, and supported options.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "bridge.h"
#include <uci.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool name_valid(const char *name)
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

static bool vlan_evidence(int root, const char *name, char parent[16], uint32_t *vid)
{
	char path[128];
	char text[4096];
	char *field;
	char *end;
	char *line;
	size_t length;
	unsigned long value;
	const char *dot = strchr(name, '.');
	if (dot == NULL || dot == name || strchr(dot + 1, '.') != NULL || !isdigit((unsigned char)dot[1])) {
		return false;
	}
	errno = 0;
	value = strtoul(dot + 1, &end, 10);
	if (errno != 0 || *end != '\0' || value == 0 || value > 4094) {
		return false;
	}
	*vid = (uint32_t)value;
	snprintf(path, sizeof(path), "proc/net/vlan/%s", name);
	if (mesh_read_bounded_at(root, path, text, sizeof(text) - 1, &length, NULL) != 0 || memchr(text, '\0', length) != NULL) {
		return false;
	}
	text[length] = '\0';
	field = strstr(text, "VID:");
	if (field == NULL || strstr(field + 4, "VID:") != NULL) {
		return false;
	}
	errno = 0;
	value = strtoul(field + 4, &end, 10);
	if (errno != 0 || value != *vid || (*end != ' ' && *end != '\n')) {
		return false;
	}
	line = strstr(text, "\nDevice:");
	if (line == NULL || strstr(line + 1, "\nDevice:") != NULL) {
		return false;
	}
	line += 8;
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	length = strcspn(line, " \t\r\n");
	if (length == 0 || length >= 16 || (size_t)(dot - name) != length || memcmp(line, name, length) != 0) {
		return false;
	}
	memcpy(parent, line, length);
	parent[length] = '\0';
	return true;
}

bool mesh_bridge_links_disjoint(int root, const char *a, const char *b)
{
	size_t first = strcspn(a, ".");
	size_t second = strcspn(b, ".");
	char parent_a[16];
	char parent_b[16];
	uint32_t vid_a;
	uint32_t vid_b;
	if (first != second || memcmp(a, b, first) != 0) {
		return true;
	}
	/* Different names alone do NOT prove isolated VLANs. Classic swconfig
	 * eth0.1/eth0.2 is allowed only with matching kernel VID/lower-device proof.
	 * A physical parent or an unproved/shared VLAN always conflicts. */
	return strcmp(a, b) != 0 && vlan_evidence(root, a, parent_a, &vid_a) &&
		   vlan_evidence(root, b, parent_b, &vid_b) && strcmp(parent_a, parent_b) == 0 && vid_a != vid_b;
}

static bool overlap(int root, const char *a, const char *b)
{
	return !mesh_bridge_links_disjoint(root, a, b);
}

static bool scalar_option(const char *name, bool *boolean)
{
	static const char *const bools[] = { "igmp_snooping", "multicast_querier", "ipv6", "multicast", "promisc" };
	static const char *const integers[] = { "mtu", "mtu6", "txqueuelen", "forward_delay", "priority", "ageing_time", "hello_time", "max_age", "hash_max", "robustness", "query_interval", "query_response_interval", "last_member_interval" };
	size_t i;
	for (i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
		if (strcmp(name, bools[i]) == 0) {
			*boolean = true;
			return true;
		}
	}
	for (i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
		if (strcmp(name, integers[i]) == 0) {
			*boolean = false;
			return true;
		}
	}
	return false;
}

static int parse_device(int root, struct mesh_bridge_config *result, struct uci_section *device)
{
	struct uci_element *element;
	struct uci_element *item;
	struct uci_option *option;
	const char *name;
	const char *value;
	char *end;
	unsigned long number;
	bool boolean;
	size_t i;
	uci_foreach_element(&device->options, element) {
		option = uci_to_option(element);
		name = element->name;
		if (strcmp(name, "ports") == 0) {
			if (option->type != UCI_TYPE_LIST) {
				return -1;
			}
			uci_foreach_element(&option->v.list, item) {
				if (!name_valid(item->name) || result->port_count == MESH_BRIDGE_PORTS ||
					overlap(root, item->name, result->name) || strcmp(item->name, "lo") == 0 ||
					(result->wan[0] != '\0' && overlap(root, item->name, result->wan))) {
					return -1;
				}
				for (i = 0; i < result->port_count; i++) {
					if (overlap(root, item->name, result->ports[i])) {
						return -1;
					}
				}
				memcpy(result->ports[result->port_count++], item->name, strlen(item->name) + 1);
			}
			continue;
		}
		if (option->type != UCI_TYPE_STRING) {
			return -1;
		}
		value = option->v.string;
		if (strcmp(name, "name") == 0 || strcmp(name, "type") == 0) {
			continue;
		}
		if (strcmp(name, "macaddr") == 0) {
			if (mesh_mac_parse(value, result->configured_mac) != MESH_OK) {
				return -1;
			}
			result->mac_set = 1;
			continue;
		}
		if (strcmp(name, "stp") == 0 || strcmp(name, "bridge_empty") == 0 || strcmp(name, "vlan_filtering") == 0) {
			if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
				return -1;
			}
			if (strcmp(name, "vlan_filtering") == 0 && strcmp(value, "0") != 0) {
				return -1;
			}
			if (strcmp(name, "stp") == 0) {
				result->stp = (uint8_t)(strcmp(value, "1") == 0);
			}
			/* stp and bridge_empty are intentionally owned runtime overrides.
			 * Their original committed values remain untouched for withdrawal. */
			continue;
		}
		if (!scalar_option(name, &boolean) || result->option_count == MESH_BRIDGE_OPTIONS ||
			strlen(name) >= sizeof(result->options[0].name) || value[0] == '\0' || !isdigit((unsigned char)value[0])) {
			return -1;
		}
		errno = 0;
		number = strtoul(value, &end, 10);
		if (errno != 0 || *end != '\0' || number > UINT32_MAX || (boolean && number > 1)) {
			return -1;
		}
		memcpy(result->options[result->option_count].name, name, strlen(name) + 1);
		result->options[result->option_count].value = (uint32_t)number;
		result->options[result->option_count++].boolean = boolean;
	}
	return 0;
}

int mesh_bridge_config_read(int root, const struct mesh_native_config *native,
							struct mesh_bridge_config *output, struct mesh_file_stamp *stamp)
{
	char contents[MESH_INPUT_MAX + 1];
	struct mesh_bridge_config result = {0};
	struct uci_context *context = NULL;
	struct uci_package *package = NULL;
	struct uci_section *selected = NULL;
	struct uci_section *section;
	struct uci_element *element;
	struct uci_element *port;
	struct uci_option *ports;
	const char *name;
	const char *value;
	size_t length;
	size_t i;
	FILE *stream = NULL;
	int status = -1;

	if (native == NULL || !name_valid(native->bridge_ifname) ||
		(native->wan_device[0] != '\0' && (!name_valid(native->wan_device) || overlap(root, native->bridge_ifname, native->wan_device)))) {
		goto done;
	}
	memcpy(result.name, native->bridge_ifname, sizeof(result.name));
	memcpy(result.network, native->network, sizeof(result.network));
	memcpy(result.mesh_ifname, native->mesh_ifname, sizeof(result.mesh_ifname));
	memcpy(result.wan, native->wan_device, sizeof(result.wan));
	if (mesh_read_bounded_at(root, "etc/config/network", contents, sizeof(contents) - 1, &length, stamp) != 0 ||
		memchr(contents, '\0', length) != NULL) {
		goto done;
	}
	context = uci_alloc_context();
	stream = fmemopen(contents, length, "r");
	if (context == NULL || stream == NULL) {
		goto done;
	}
	context->flags = UCI_FLAG_STRICT;
	if (uci_import(context, stream, "network", &package, true) != UCI_OK) {
		goto done;
	}
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		name = uci_lookup_option_string(context, section, "name");
		if (strcmp(section->e.name, MESH_BRIDGE_ANCHOR) == 0) {
			goto done;
		}
		if (strcmp(section->type, "device") == 0 && name != NULL && strcmp(name, result.name) == 0) {
			value = uci_lookup_option_string(context, section, "type");
			if (selected != NULL || value == NULL || strcmp(value, "bridge") != 0) {
				goto done;
			}
			selected = section;
		}
		value = uci_lookup_option_string(context, section, "device");
		if ((strcmp(section->type, "bridge-vlan") == 0 && value != NULL && strcmp(value, result.name) == 0) ||
			(strcmp(section->type, "interface") == 0 && strcmp(section->e.name, result.network) != 0 &&
			 value != NULL && (strcmp(value, result.name) == 0 || (value[0] == '@' && strcmp(value + 1, result.network) == 0)))) {
			goto done;
		}
	}
	if (selected == NULL || parse_device(root, &result, selected) != 0) {
		goto done;
	}
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		if (section == selected || strcmp(section->type, "device") != 0) {
			continue;
		}
		name = uci_lookup_option_string(context, section, "name");
		value = uci_lookup_option_string(context, section, "ifname");
		if (value != NULL) {
			for (i = 0; i < result.port_count; i++) {
				if (overlap(root, value, result.ports[i]) || (name != NULL && strcmp(name, result.ports[i]) == 0)) {
					goto done;
				}
			}
		}
		ports = uci_lookup_option(context, section, "ports");
		if (ports != NULL && ports->type == UCI_TYPE_LIST) {
			uci_foreach_element(&ports->v.list, port) {
				for (i = 0; i < result.port_count; i++) {
					if (overlap(root, port->name, result.ports[i])) {
						goto done;
					}
				}
			}
		}
	}
	uci_foreach_element(&package->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "interface") != 0 || strcmp(section->e.name, result.network) == 0) {
			continue;
		}
		value = uci_lookup_option_string(context, section, "device");
		if (value != NULL && value[0] != '@') {
			for (i = 0; i < result.port_count; i++) {
				if (overlap(root, value, result.ports[i])) {
					goto done;
				}
			}
		}
	}
	if (mesh_stamp_unchanged(root, stamp) != 0) {
		goto done;
	}
	*output = result;
	status = 0;
done:
	if (stream != NULL) {
		fclose(stream);
	}
	if (context != NULL) {
		uci_free_context(context);
	}
	mesh_sensitive_clear(contents, sizeof(contents));
	return status;
}
