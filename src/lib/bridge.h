/**
 * @file bridge.h
 * @brief Owned bridge snapshot, options, and recovery-phase types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_BRIDGE_H
#define MESH_BRIDGE_H
#include "peer.h"
#define MESH_BRIDGE_SERVICE "mesh11sd-v8-bridge"
#define MESH_BRIDGE_ANCHOR "mesh11sd_v8_bridge_anchor"
#define MESH_BRIDGE_PORTS 32
#define MESH_BRIDGE_OPTIONS 32

struct mesh_bridge_option {
	char name[32];
	uint32_t value;
	uint8_t boolean;
};

/* Public, finite bridge parameters only. No UCI package or credential blobs. */
struct mesh_bridge_config {
	char name[16];
	char network[32];
	char mesh_ifname[16];
	char wan[16];
	char ports[MESH_BRIDGE_PORTS][16];
	uint32_t port_count;
	struct mesh_bridge_option options[MESH_BRIDGE_OPTIONS];
	uint32_t option_count;
	uint8_t configured_mac[6];
	uint8_t mac_set;
	uint8_t stp;
};

struct mesh_bridge_disk {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t phase;
	char owner[33];
	struct mesh_bridge_config config;
	uint8_t original_present;
	uint8_t original_mac[6];
	uint8_t actual_mac[6];
	uint8_t pinned;
};

struct mesh_bridge {
	struct mesh_peer *peer;
	int root;
	struct mesh_bridge_disk disk;
	struct mesh_runtime_data installed;
	struct mesh_file_stamp network_stamp;
	struct mesh_file_stamp journal_stamp;
	bool journal_exists;
	bool present;
	bool recovering;
	bool released_changed_inputs;
};

bool mesh_bridge_links_disjoint(int root, const char *a, const char *b);
int mesh_bridge_ports_guard(struct mesh_bridge *bridge);
int mesh_bridge_probe(struct mesh_bridge *bridge, uint8_t mac[6], bool *stp, bool require_ports);
int mesh_bridge_config_read(int root, const struct mesh_native_config *native,
							struct mesh_bridge_config *output, struct mesh_file_stamp *stamp);
int mesh_bridge_open(struct mesh_bridge *bridge, struct mesh_peer *peer, int root,
					 const struct mesh_native_config *config);
int mesh_bridge_apply(struct mesh_bridge *bridge);
int mesh_bridge_verify(struct mesh_bridge *bridge);
/* Must run ONLY after peer withdrawal and role-service restoration. */
int mesh_bridge_stop(struct mesh_bridge *bridge);
void mesh_bridge_clear(struct mesh_bridge *bridge);
int mesh_bridge_payload(const struct mesh_bridge_disk *state, bool pinned,
						struct mesh_runtime_data *output);
#endif
