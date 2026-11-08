/**
 * @file factory.c
 * @brief Select a factory MAC from the six-stage supplied-evidence chain.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

/* Independent mesh11sd identity policy, kept in agreement with apmond's
 * six-stage factory-MAC chain. No monitoring/signing/collector functionality. */
#include "factory.h"

#include <string.h>

static bool evidence_valid(const struct mesh_mac_evidence *evidence)
{
	return evidence->present && mesh_mac_valid(evidence->mac);
}

static const uint8_t *permanent_mac(const struct mesh_factory_interface *interface)
{
	if (evidence_valid(&interface->ethtool_permanent)) {
		return interface->ethtool_permanent.mac;
	}
	/* NET_ADDR_PERM == 0. A rewritten address is not factory identity. */
	if (interface->addr_assign_type == 0 && evidence_valid(&interface->sysfs_address)) {
		return interface->sysfs_address.mac;
	}
	return NULL;
}

static const struct mesh_factory_interface *find_interface(const struct mesh_factory_input *input,
														   const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) >= 16 || strchr(name, '/') != NULL) {
		return NULL;
	}
	for (i = 0; i < input->interface_count; i++) {
		if (strcmp(input->interfaces[i].name, name) == 0) {
			return &input->interfaces[i];
		}
	}
	return NULL;
}

static enum mesh_error choose(struct mesh_factory_result *output, const uint8_t mac[6],
							  enum mesh_factory_source source, const char *name)
{
	struct mesh_factory_result result;

	memset(&result, 0, sizeof(result));
	memcpy(result.mac, mac, 6);
	result.source = source;
	if (name != NULL) {
		memcpy(result.interface_name, name, strlen(name) + 1);
	}
	*output = result;
	return MESH_OK;
}

enum mesh_error mesh11sd_factory_mac(const struct mesh_factory_input *input,
									 struct mesh_factory_result *result)
{
	static const char *const common[] = { "eth1", "eth0", "lan0", "lan1" };
	const struct mesh_factory_interface *interface;
	const struct mesh_factory_interface *best;
	const uint8_t *mac;
	const uint8_t *best_mac;
	size_t i;
	size_t j;

	if (input == NULL || result == NULL || input->interface_count > MESH_FACTORY_INTERFACES_MAX) {
		return MESH_E_INPUT;
	}
	for (i = 0; i < input->interface_count; i++) {
		if (memchr(input->interfaces[i].name, '\0', sizeof(input->interfaces[i].name)) == NULL ||
			input->interfaces[i].name[0] == '\0' || input->interfaces[i].ifindex == 0 ||
			strchr(input->interfaces[i].name, '/') != NULL) {
			return MESH_E_INPUT;
		}
		for (j = 0; j < i; j++) {
			if (input->interfaces[i].ifindex == input->interfaces[j].ifindex ||
				strcmp(input->interfaces[i].name, input->interfaces[j].name) == 0) {
				return MESH_E_INPUT;
			}
		}
	}
	for (i = 0; i < 3; i++) {
		if (evidence_valid(&input->device_tree[i].mac_address)) {
			return choose(result, input->device_tree[i].mac_address.mac,
						  (enum mesh_factory_source)i, NULL);
		}
		if (evidence_valid(&input->device_tree[i].local_mac_address)) {
			return choose(result, input->device_tree[i].local_mac_address.mac,
						  (enum mesh_factory_source)i, NULL);
		}
	}
	if (evidence_valid(&input->board_json_system_label)) {
		return choose(result, input->board_json_system_label.mac, MESH_FACTORY_BOARD, NULL);
	}
	if (evidence_valid(&input->first_uci_system_label)) {
		return choose(result, input->first_uci_system_label.mac, MESH_FACTORY_SYSTEM, NULL);
	}
	interface = find_interface(input, input->wan_device);
	if (interface != NULL && !interface->is_bridge) {
		mac = permanent_mac(interface);
		if (mac != NULL) {
			return choose(result, mac, MESH_FACTORY_WAN, interface->name);
		}
	}
	for (i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
		interface = find_interface(input, common[i]);
		if (interface == NULL || interface->is_bridge) {
			continue;
		}
		mac = permanent_mac(interface);
		if (mac != NULL && (mac[0] & 2) == 0) {
			return choose(result, mac, MESH_FACTORY_COMMON, interface->name);
		}
	}
	best = NULL;
	best_mac = NULL;
	for (i = 0; i < input->interface_count; i++) {
		interface = &input->interfaces[i];
		if (interface->arphrd_type != 1 || interface->is_bridge) {
			continue;
		}
		mac = permanent_mac(interface);
		if (mac == NULL || (mac[0] & 2) != 0) {
			continue;
		}
		if (best == NULL || interface->ifindex < best->ifindex) {
			best = interface;
			best_mac = mac;
		}
	}
	if (best != NULL) {
		return choose(result, best_mac, MESH_FACTORY_LOWEST_IFINDEX, best->name);
	}
	return MESH_E_IDENTITY;
}


enum mesh_error mesh11sd_plan_from_evidence(const struct mesh_config *config,
										   const struct mesh_factory_input *identity_input,
										   const struct mesh_facts *platform_facts,
										   struct mesh_plan *plan)
{
	struct mesh_factory_result identity;
	struct mesh_facts facts;
	enum mesh_error error;

	if (platform_facts == NULL || config == NULL || plan == NULL) {
		return MESH_E_INPUT;
	}
	error = mesh11sd_factory_mac(identity_input, &identity);
	if (error != MESH_OK) {
		return error;
	}
	facts = *platform_facts;
	memcpy(facts.factory_mac, identity.mac, 6);
	return mesh_plan_build(config, &facts, plan);
}
