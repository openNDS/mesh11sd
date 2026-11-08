/**
 * @file native_config.c
 * @brief Import the five committed UCI files through libuci.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "native.h"
#include "sha256.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>
#include <unistd.h>

static const char *option(struct uci_context *context, struct uci_section *section,
						  const char *name)
{
	return section != NULL ? uci_lookup_option_string(context, section, name) : NULL;
}

static bool copy_string(char *out, size_t capacity, const char *text)
{
	if (text == NULL || strlen(text) >= capacity) {
		return false;
	}
	memcpy(out, text, strlen(text) + 1);
	return true;
}

static bool integer(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
	unsigned long parsed;
	char *end;
	size_t i;

	if (text == NULL || text[0] == '\0') {
		return false;
	}
	for (i = 0; text[i] != '\0'; i++) {
		if (!isdigit((unsigned char)text[i])) {
			return false;
		}
	}
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
		return false;
	}
	*out = (uint32_t)parsed;
	return true;
}

static struct uci_package *committed_package(int root_fd, const char *name,
											 struct uci_context *context,
											 struct mesh_file_stamp *stamp)
{
	unsigned char buffer[MESH_INPUT_MAX + 1];
	struct uci_package *package;
	char path[40];
	size_t length;
	FILE *stream;
	int status;

	snprintf(path, sizeof(path), "etc/config/%s", name);
	if (mesh_read_bounded_at(root_fd, path, buffer, MESH_INPUT_MAX, &length, stamp) != 0 ||
		memchr(buffer, '\0', length) != NULL) {
		mesh_sensitive_clear(buffer, sizeof(buffer));
		return NULL;
	}
	buffer[length] = '\0';
	stream = fmemopen(buffer, length, "r");
	package = NULL;
	status = stream != NULL ? uci_import(context, stream, name, &package, true) : UCI_ERR_IO;
	if (stream != NULL) {
		fclose(stream);
	}
	mesh_sensitive_clear(buffer, sizeof(buffer));
	return status == UCI_OK ? package : NULL;
}

static uint32_t frequency(const char *band, uint32_t channel)
{
	if (strcmp(band, "2g") == 0) {
		if (channel >= 1 && channel <= 13) {
			return 2407 + channel * 5;
		}
		return channel == 14 ? 2484 : 0;
	}
	if (strcmp(band, "5g") == 0 && channel >= 1 && channel <= 196) {
		return 5000 + channel * 5;
	}
	if (strcmp(band, "6g") == 0 && channel >= 1 && channel <= 233) {
		return channel == 2 ? 5935 : 5950 + channel * 5;
	}
	return 0;
}

static int resolve_phy(int root_fd, const char *path, const char *explicit_phy,
					   char output[16])
{
	char link_path[128];
	char target[512];
	struct dirent *entry;
	DIR *directory;
	ssize_t length;
	int fd;
	int result;
	size_t path_length;
	uint32_t index;

	if (explicit_phy != NULL) {
		if (strncmp(explicit_phy, "phy", 3) != 0 ||
			!integer(explicit_phy + 3, 0, 65535, &index) ||
			!copy_string(output, 16, explicit_phy)) {
			return -1;
		}
		return 0;
	}
	if (path == NULL || path[0] == '\0') {
		return -1;
	}
	fd = mesh_open_at_root(root_fd, "sys/class/ieee80211", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	directory = fdopendir(fd);
	if (directory == NULL) {
		close(fd);
		return -1;
	}
	result = -1;
	path_length = strlen(path);
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, "phy", 3) != 0 ||
			!integer(entry->d_name + 3, 0, 65535, &index) || strlen(entry->d_name) >= 16) {
			continue;
		}
		snprintf(link_path, sizeof(link_path), "sys/class/ieee80211/%.15s/device", entry->d_name);
		length = mesh_readlink_at_root(root_fd, link_path, target, sizeof(target) - 1);
		if (length < 0 || (size_t)length >= sizeof(target) - 1 || (size_t)length < path_length) {
			continue;
		}
		target[length] = '\0';
		if (strcmp(target + (size_t)length - path_length, path) != 0 ||
			((size_t)length > path_length && target[(size_t)length - path_length - 1] != '/')) {
			continue;
		}
		if (result == 0) {
			result = -1;
			break;
		}
		(void)copy_string(output, 16, entry->d_name);
		result = 0;
	}
	closedir(directory);
	return result;
}

static bool driver_beacon_workaround(int root_fd, const char *phy)
{
	char path[128];
	char target[512];
	char *driver;
	ssize_t length;

	snprintf(path, sizeof(path), "sys/class/ieee80211/%s/device/driver", phy);
	length = mesh_readlink_at_root(root_fd, path, target, sizeof(target) - 1);
	if (length <= 0 || (size_t)length >= sizeof(target) - 1) {
		return false;
	}
	target[length] = '\0';
	driver = strrchr(target, '/');
	driver = driver != NULL ? driver + 1 : target;
	/* v7 mobility_settings 4388-4401, not a driver-name-to-PHY guess. */
	return strncmp(driver, "ath11k", 6) == 0 || strcmp(driver, "mt76_wmac") == 0;
}

static int choose_radio(int root_fd, struct uci_context *context, struct uci_package *wireless,
						struct uci_section *setup, struct mesh_native_config *result)
{
	const char *wanted;
	const char *selected_index;
	const char *band;
	const char *value;
	const char *country;
	const char *base;
	struct uci_element *element;
	struct uci_section *section;
	char requested_band[8];
	char prefix[8];
	char phy[16];
	uint32_t channel;
	uint32_t phy_index;
	uint32_t radio_index;
	uint32_t preference;
	size_t i;
	int written;

	wanted = option(context, setup, "auto_mesh_band");
	if (wanted == NULL) {
		wanted = "2g40";
	}
	if (strcmp(wanted, "2g40") == 0) {
		wanted = "2g";
	}
	if (!copy_string(requested_band, sizeof(requested_band), wanted) ||
		(strcmp(wanted, "2g") != 0 && strcmp(wanted, "5g") != 0 && strcmp(wanted, "6g") != 0)) {
		return -1;
	}
	selected_index = option(context, setup, "mesh_phy_index");
	preference = UINT32_MAX;
	if (selected_index != NULL && !integer(selected_index, 0, 65535, &preference)) {
		return -1;
	}
	value = option(context, setup, "mesh_multi_interfaces");
	if (value != NULL && strcmp(value, "disabled") != 0) {
		return -1;
	}
	uci_foreach_element(&wireless->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "wifi-device") != 0) {
			continue;
		}
		band = option(context, section, "band");
		if (band == NULL || strcmp(band, requested_band) != 0) {
			continue;
		}
		if (resolve_phy(root_fd, option(context, section, "path"), option(context, section, "phy"), phy) < 0 ||
			!integer(phy + 3, 0, 65535, &phy_index)) {
			continue;
		}
		if (preference != UINT32_MAX && phy_index != preference) {
			continue;
		}
		value = option(context, setup, "portal_channel");
		if (value != NULL && strcmp(value, "default") != 0) {
			/* Automatic per-portal choice needs channel discovery/migration;
			 * do not pretend a fixed channel implements that lifecycle. */
			if (strcmp(band, "2g") != 0 || !integer(value, 1, 13, &channel)) {
				return -1;
			}
		} else {
			value = option(context, section, "channel");
		}
		if (!integer(value, 1, 233, &channel) || frequency(band, channel) == 0 ||
			strncmp(section->e.name, "radio", 5) != 0 ||
			!integer(section->e.name + 5, 0, 255, &radio_index)) {
			return -1;
		}
		result->beacon_workaround |= driver_beacon_workaround(root_fd, phy);
		result->radio_channel = channel;
		result->radio_beacon_interval = 100;
		value = option(context, section, "beacon_int");
		if (value != NULL && !integer(value, 15, 65535, &result->radio_beacon_interval)) {
			return -1;
		}
		result->config.frequency_mhz = frequency(band, channel);
		(void)copy_string(result->phy, sizeof(result->phy), phy);
		if (!copy_string(result->radio_section, sizeof(result->radio_section), section->e.name) ||
			!copy_string(result->htmode, sizeof(result->htmode), option(context, section, "htmode"))) {
			return -1;
		}
		for (i = 0; result->htmode[i] != '\0'; i++) {
			if (!isalnum((unsigned char)result->htmode[i])) {
				return -1;
			}
		}
		country = option(context, setup, "country");
		if (country == NULL) {
			country = option(context, section, "country");
		}
		if (country != NULL) {
			if (strlen(country) != 2 ||
				!((isupper((unsigned char)country[0]) && isupper((unsigned char)country[1])) ||
				  strcmp(country, "00") == 0)) {
				return -1;
			}
			memcpy(result->country, country, 3);
		}
		base = option(context, setup, "mesh_basename");
		if (base == NULL) {
			memcpy(prefix, "m-11s-", 7);
		} else {
			if (strlen(base) == 0 || strlen(base) > 4) {
				return -1;
			}
			memset(prefix, 0, sizeof(prefix));
			memcpy(prefix, "m-", 2);
			for (i = 0; base[i] != '\0'; i++) {
				if (!isalnum((unsigned char)base[i])) {
					return -1;
				}
				prefix[2 + i] = (char)tolower((unsigned char)base[i]);
			}
			prefix[2 + i] = '-';
		}
		written = snprintf(result->mesh_ifname, sizeof(result->mesh_ifname), "%sr%xp%x", prefix,
						   (unsigned int)radio_index, (unsigned int)phy_index);
		return written > 0 && (size_t)written < sizeof(result->mesh_ifname) ? 0 : -1;
	}
	return -1;
}

static int common_ipv4(const char *on_air_id, uint32_t *address)
{
	char digest[65];
	char pair[3];
	char *end;
	unsigned long subnet;
	size_t i;

	sha256_hex((const unsigned char *)on_air_id, strlen(on_air_id), digest);
	for (i = 9; i <= 64; i++) {
		memcpy(pair, digest + 64 - i, 2);
		pair[2] = '\0';
		subnet = strtoul(pair, &end, 16);
		if (*end != '\0' || subnet == 0 || subnet == 1 || subnet == 8 ||
			subnet == 167 || subnet == 168 || subnet == 255) {
			continue;
		}
		*address = 0xc0a80001U | ((uint32_t)subnet << 8);
		mesh_sensitive_clear(digest, sizeof(digest));
		return 0;
	}
	mesh_sensitive_clear(digest, sizeof(digest));
	return -1;
}

static int read_pool(struct uci_context *context, struct uci_package *network,
					 struct uci_package *dhcp, struct uci_section *setup,
					 struct mesh_native_config *result)
{
	struct uci_section *lan;
	struct uci_section *pool;
	const char *value;
	struct in_addr address;
	struct in_addr mask;
	uint32_t start;
	uint32_t limit;
	uint32_t seconds;
	char lease[32];
	size_t length;
	unsigned int shift;

	lan = uci_lookup_section(context, network, result->network);
	pool = uci_lookup_section(context, dhcp, result->network);
	if (lan == NULL || pool == NULL ||
		!integer(option(context, pool, "start"), 1, 254, &start) ||
		!integer(option(context, pool, "limit"), 1, 253, &limit) || start + limit > 255) {
		return -1;
	}
	value = option(context, setup, "portal_use_default_ipv4");
	result->config.portal_ipv4.prefix = 24;
	if (value != NULL && strcmp(value, "1") == 0) {
		value = option(context, lan, "ipaddr");
		if (value == NULL || inet_pton(AF_INET, value, &address) != 1) {
			return -1;
		}
		result->config.portal_ipv4.address = ntohl(address.s_addr);
		value = option(context, lan, "netmask");
		if (value == NULL || inet_pton(AF_INET, value, &mask) != 1 || ntohl(mask.s_addr) != 0xffffff00U) {
			/* More prefixes need generalized offset handling, not /24 guessing. */
			return -1;
		}
	} else if (value != NULL && strcmp(value, "0") != 0) {
		return -1;
	} else if (common_ipv4(result->on_air_id, &result->config.portal_ipv4.address) < 0) {
		return -1;
	}
	result->config.portal_ipv4.pool_first = (result->config.portal_ipv4.address & 0xffffff00U) + start;
	result->config.portal_ipv4.pool_last = result->config.portal_ipv4.pool_first + limit - 1;
	value = option(context, pool, "leasetime");
	seconds = 3600;
	if (value != NULL) {
		if (!copy_string(lease, sizeof(lease), value)) {
			return -1;
		}
		length = strlen(lease);
		shift = 1;
		if (length != 0 && lease[length - 1] == 'h') {
			shift = 3600;
			lease[--length] = '\0';
		} else if (length != 0 && lease[length - 1] == 'm') {
			shift = 60;
			lease[--length] = '\0';
		} else if (length != 0 && lease[length - 1] == 's') {
			lease[--length] = '\0';
		}
		if (!integer(lease, 1, UINT32_MAX / shift, &seconds)) {
			return -1;
		}
		seconds *= shift;
	}
	result->config.portal_ipv4.lease_seconds = seconds;
	return 0;
}

static void rebind(struct mesh_native_config *config)
{
	config->config.auto_mesh_id = config->group;
	config->config.mesh_id = config->on_air_id;
	config->config.preferred_phy = config->phy;
	config->config.mesh_ifname = config->mesh_ifname;
	config->config.bridge_ifname = config->bridge_ifname;
}

void mesh_native_config_clear(struct mesh_native_config *config)
{
	if (config != NULL) {
		mesh_sensitive_clear(config, sizeof(*config));
	}
}

/* Already-quiesced path; other configurations require the scoped runtime
 * service handoff before mesh release, never a silent active LAN server. */
static bool peer_services_ready(struct uci_context *context, struct uci_package *dhcp, const char *network)
{
	static const char *const modes[] = { "ra", "dhcpv4", "dhcpv6", "ndp" };
	struct uci_element *element;
	struct uci_section *section;
	const char *value;
	size_t i;
	bool found = false;

	uci_foreach_element(&dhcp->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "dhcp") != 0) {
			continue;
		}
		value = option(context, section, "interface");
		if (strcmp(section->e.name, network) != 0 && (value == NULL || strcmp(value, network) != 0)) {
			continue;
		}
		found = true;
		value = option(context, section, "ignore");
		if (value == NULL || strcmp(value, "1") != 0) {
			return false;
		}
		for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			value = option(context, section, modes[i]);
			if (value != NULL && strcmp(value, "disabled") != 0) {
				return false;
			}
		}
	}
	return found;
}

static int service_owners(struct uci_context *context, struct uci_package *dhcp,
						  struct mesh_native_config *result)
{
	struct uci_section *pool;
	struct uci_section *section;
	struct uci_element *pool_element;
	struct uci_element *element;
	const char *instance;
	const char *value;
	size_t i;
	bool already;
	bool found = false;

	result->odhcpd_ignored = true;
	uci_foreach_element(&dhcp->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->type, "odhcpd") == 0) {
			value = option(context, section, "maindhcp");
			if (value != NULL && strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
				return -1;
			}
			result->odhcpd_maindhcp = result->odhcpd_maindhcp || (value != NULL && strcmp(value, "1") == 0);
		}
	}
	uci_foreach_element(&dhcp->sections, pool_element) {
		pool = uci_to_section(pool_element);
		value = option(context, pool, "interface");
		if (strcmp(pool->type, "dhcp") != 0 ||
			(strcmp(pool->e.name, result->network) != 0 && (value == NULL || strcmp(value, result->network) != 0))) {
			continue;
		}
		found = true;
		value = option(context, pool, "ignore");
		if (value != NULL && strcmp(value, "1") == 0) {
			continue;
		}
		result->odhcpd_ignored = false;
		result->dns_suppression_needed = true;
		instance = option(context, pool, "instance");
		uci_foreach_element(&dhcp->sections, element) {
			section = uci_to_section(element);
			if (strcmp(section->type, "dnsmasq") != 0) {
				continue;
			}
			value = option(context, section, "disabled");
			if ((value != NULL && strcmp(value, "1") == 0) ||
				(instance != NULL && strcmp(instance, section->e.name) != 0)) {
				continue;
			}
			/* filter_dnsmasq: every active LAN-bound pool can select a different
			 * existing owner; unbound pools apply to EVERY enabled instance. */
			already = false;
			for (i = 0; i < result->dns_instance_count; i++) {
				already = already || strcmp(result->dns_instances[i], section->e.name) == 0;
			}
			if (already) {
				continue;
			}
			if (result->dns_instance_count == MESH_DNS_OWNERS_MAX ||
				!copy_string(result->dns_instances[result->dns_instance_count],
							 sizeof(result->dns_instances[0]), section->e.name)) {
				return -1;
			}
			result->dns_instance_count++;
		}
	}
	return found ? 0 : -1;
}

enum mesh_error mesh_native_config_read(int root_fd, struct mesh_native_config *output)
{
	static const char default_salt[] = "91b39a9b41e918e9bce1c8d5d7d3e071d6f1f8855a7c0214f687550177c5d5b8";
	struct mesh_native_config result;
	struct uci_context *context;
	struct uci_package *packages[5];
	struct uci_section *setup;
	struct uci_section *lan;
	struct uci_section *wan;
	struct uci_element *element;
	struct uci_section *section;
	const char *device;
	const char *mode;
	static const char *const names[] = { "mesh11sd", "network", "wireless", "dhcp", "system" };
	const char *value;
	char digest[65];
	char key_material[256];
	uint32_t number;
	size_t i;
	int written;
	enum mesh_error error;

	if (output == NULL) {
		return MESH_E_INPUT;
	}
	memset(&result, 0, sizeof(result));
	memset(key_material, 0, sizeof(key_material));
	memset(digest, 0, sizeof(digest));
	context = uci_alloc_context();
	if (context == NULL) {
		return MESH_E_PLATFORM;
	}
	context->flags = UCI_FLAG_STRICT;
	error = MESH_E_INPUT;
	for (i = 0; i < 5; i++) {
		packages[i] = committed_package(root_fd, names[i], context, &result.stamps[i]);
		if (packages[i] == NULL) {
			goto done;
		}
		result.stamp_count++;
	}
	setup = uci_lookup_section(context, packages[0], "setup");
	value = option(context, setup, "auto_config");
	number = 0;
	if (value != NULL && !integer(value, 0, 2, &number)) {
		goto done;
	}
	if (number == 2) {
		/* Locked/manual mode must preserve its complete existing definition. */
		goto done;
	}
	result.enabled = number == 1;
	number = 0;
	value = option(context, setup, "use_default_beacon_interval");
	if (value != NULL && !integer(value, 0, 1, &number)) {
		goto done;
	}
	result.beacon_workaround = number != 0;
	number = 1;
	value = option(context, setup, "portal_detect");
	if (value != NULL && !integer(value, 0, 5, &number)) {
		goto done;
	}
	result.portal_detect = number;
	value = option(context, setup, "auto_mesh_id");
	if (!copy_string(result.group, sizeof(result.group), value != NULL ? value : "--__") ||
		result.group[0] == '\0') {
		goto done;
	}
	sha256_hex((const unsigned char *)result.group, strlen(result.group), digest);
	memcpy(result.on_air_id, digest + 9, 30);
	value = option(context, setup, "auto_mesh_key");
	written = snprintf(key_material, sizeof(key_material), "%s%s\n", result.on_air_id,
					   value != NULL && value[0] != '\0' ? value : default_salt);
	if (written < 0 || (size_t)written >= sizeof(key_material)) {
		goto done;
	}
	sha256_hex((const unsigned char *)key_material, (size_t)written, result.credential);
	result.config.security = MESH_SECURITY_SAE;
	result.config.credential_available = true;
	value = option(context, setup, "auto_mesh_network");
	if (!copy_string(result.network, sizeof(result.network), value != NULL ? value : "lan")) {
		goto done;
	}
	for (i = 0; result.network[i] != '\0'; i++) {
		if (!isalnum((unsigned char)result.network[i]) && result.network[i] != '_') {
			goto done;
		}
	}
	if ((uci_lookup_section(context, packages[1], "mesh11sd_v8_management") != NULL ||
		 uci_lookup_section(context, packages[1], "mesh11sd_v8_client") != NULL)) {
		goto done;
	}
	lan = uci_lookup_section(context, packages[1], result.network);
	value = option(context, lan, "device");
	if (!copy_string(result.bridge_ifname, sizeof(result.bridge_ifname), value)) {
		goto done;
	}
	wan = uci_lookup_section(context, packages[1], "wan");
	value = option(context, wan, "device");
	if (value != NULL && !copy_string(result.wan_device, sizeof(result.wan_device), value)) {
		goto done;
	}
	error = MESH_E_RADIO;
	if (choose_radio(root_fd, context, packages[2], setup, &result) != 0) {
		goto done;
	}
	/* Enabling a currently disabled radio may also activate committed APs.
	 * Do not confuse an empty current station inventory with proof that all
	 * desired AP+mesh interface combinations are supported. */
	uci_foreach_element(&packages[2]->sections, element) {
		section = uci_to_section(element);
		if (strcmp(section->e.name, "mesh11sd_v8_mesh") == 0) {
			error = MESH_E_INPUT;
			goto done;
		}
		if (strcmp(section->type, "wifi-iface") != 0) {
			continue;
		}
		device = option(context, section, "device");
		value = option(context, section, "disabled");
		mode = option(context, section, "mode");
		if (device == NULL || strcmp(device, result.radio_section) != 0 ||
			(value != NULL && strcmp(value, "1") == 0)) {
			continue;
		}
		if (mode != NULL && strcmp(mode, "ap") == 0) {
			if (result.configured_ap_count >= MESH_APS_MAX ||
				!copy_string(result.aps[result.configured_ap_count].section,
							 sizeof(result.aps[0].section), section->e.name)) {
				goto done;
			}
			value = option(context, section, "ifname");
			if (value != NULL && !copy_string(result.aps[result.configured_ap_count].ifname,
											  sizeof(result.aps[0].ifname), value)) {
				goto done;
			}
			result.configured_ap_count++;
		} else {
			error = MESH_E_RADIO;
			goto done;
		}
	}
	number = 2;
	value = option(context, setup, "mesh_node_mobility_level");
	if (value != NULL && !integer(value, 0, 6, &number)) {
		error = MESH_E_INPUT;
		goto done;
	}
	error = mesh_mobility_read(number, false, result.beacon_workaround, &result.mobility);
	if (error != MESH_OK) {
		goto done;
	}
	error = MESH_E_POOL;
	if (read_pool(context, packages[1], packages[3], setup, &result) != 0) {
		goto done;
	}
	/* Detect a changing committed input set before returning a coherent plan. */
	result.peer_services_ready = peer_services_ready(context, packages[3], result.network);
	if (service_owners(context, packages[3], &result) != 0) {
		error = MESH_E_INPUT;
		goto done;
	}
	for (i = 0; i < result.stamp_count; i++) {
		if (mesh_stamp_unchanged(root_fd, &result.stamps[i]) < 0) {
			error = MESH_E_INPUT;
			goto done;
		}
	}
	*output = result;
	rebind(output);
	error = MESH_OK;
done:
	uci_free_context(context);
	mesh_sensitive_clear(key_material, sizeof(key_material));
	mesh_sensitive_clear(digest, sizeof(digest));
	mesh_native_config_clear(&result);
	return error;
}
