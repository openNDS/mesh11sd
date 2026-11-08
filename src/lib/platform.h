/**
 * @file platform.h
 * @brief Radio, station, and bridge-probe result types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_PLATFORM_H
#define MESH_PLATFORM_H

#include "mesh_config.h"

/* Read-only kernel queries. No UCI, child processes, sockets carrying keys,
 * network configuration mutations, or netifd apply operations. */
enum mesh_error mesh_probe_bridge(const char *ifname, uint8_t mac[6]);
enum mesh_error mesh_probe_radio(uint32_t wiphy, uint32_t frequency,
								struct mesh_radio *radio);
enum mesh_error mesh_probe_empty_radio(uint32_t wiphy, uint32_t frequency,
									  struct mesh_radio *radio);
enum mesh_error mesh_probe_wan_role(const char *wan, enum mesh_role *role);
/* Pure decoders, also exercised with hostile/truncated host fixtures. */
int mesh_decode_bridge(const void *data, size_t length, uint8_t mac[6]);
int mesh_decode_wiphy(const void *data, size_t length, uint32_t frequency,
					  struct mesh_radio *radio);

#define MESH_INTERFACES_MAX 64
struct mesh_interface {
	uint64_t wdev;
	char ifname[16];
	uint32_t wiphy;
	uint32_t ifindex;
	uint32_t iftype;
	uint32_t frequency;
};

struct mesh_interface_list {
	struct mesh_interface entries[MESH_INTERFACES_MAX];
	size_t count;
};

int mesh_decode_interface(const void *data, size_t length, struct mesh_interface *output);
enum mesh_error mesh_probe_interfaces(struct mesh_interface_list *output);
bool mesh_combination_allows(const struct mesh_radio *radio, unsigned int aps,
							 const uint32_t *beacons, size_t beacon_count);
int mesh_probe_ipv6_ready(const char *ifname, const char *address);
#endif
