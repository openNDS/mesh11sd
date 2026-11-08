/**
 * @file factory.h
 * @brief Factory-MAC evidence and selection interface.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_FACTORY_H
#define MESH_FACTORY_H

#include "mesh_config.h"

#define MESH_FACTORY_INTERFACES_MAX 64

/* Native adapters supply parsed read-only evidence, never a bridge fallback.
 * This keeps identity policy independently testable without libapmon or UCI. */
struct mesh_mac_evidence {
	bool present;
	uint8_t mac[6];
};

struct mesh_device_tree_mac {
	struct mesh_mac_evidence mac_address;
	struct mesh_mac_evidence local_mac_address;
};

struct mesh_factory_interface {
	char name[16];
	unsigned int ifindex;
	unsigned int arphrd_type;
	bool is_bridge;
	struct mesh_mac_evidence ethtool_permanent;
	struct mesh_mac_evidence sysfs_address;
	unsigned int addr_assign_type;
};

struct mesh_factory_input {
	/* sysfs DT, /proc/device-tree, mesh11sd's own pre-jail DT copy. */
	struct mesh_device_tree_mac device_tree[3];
	struct mesh_mac_evidence board_json_system_label;
	struct mesh_mac_evidence first_uci_system_label;
	const char *wan_device;
	struct mesh_factory_interface interfaces[MESH_FACTORY_INTERFACES_MAX];
	size_t interface_count;
};

enum mesh_factory_source {
	MESH_FACTORY_DT_SYSFS,
	MESH_FACTORY_DT_PROC,
	MESH_FACTORY_DT_COPY,
	MESH_FACTORY_BOARD,
	MESH_FACTORY_SYSTEM,
	MESH_FACTORY_WAN,
	MESH_FACTORY_COMMON,
	MESH_FACTORY_LOWEST_IFINDEX
};

struct mesh_factory_result {
	uint8_t mac[6];
	enum mesh_factory_source source;
	char interface_name[16];
};

/* Same six-stage policy as apmond; independent implementation, no apmond link.
 * Runtime collectors still need to fill this evidence from native interfaces. */
enum mesh_error mesh11sd_factory_mac(const struct mesh_factory_input *input,
									 struct mesh_factory_result *result);

enum mesh_error mesh11sd_plan_from_evidence(const struct mesh_config *config,
										   const struct mesh_factory_input *identity_input,
										   const struct mesh_facts *platform_facts,
										   struct mesh_plan *plan);

#endif
