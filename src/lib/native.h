/**
 * @file native.h
 * @brief Committed-input snapshot and native planner adapter interface.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_NATIVE_H
#define MESH_NATIVE_H

#include "factory.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MESH_INPUT_MAX 65536
#define MESH_INPUT_FILES 8
#define MESH_APS_MAX 16
#define MESH_DNS_OWNERS_MAX 8

struct mesh_file_stamp {
	char relative_path[128];
	dev_t device;
	ino_t inode;
	off_t size;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	struct timespec mtime;
	struct timespec ctime;
	/* Confidential change detector, memory only; never journal or log it. */
	char content_hash[65];
};

struct mesh_mobility {
	unsigned int level;
	unsigned int beacon_interval;
	unsigned int dtim_period;
	unsigned int rootmode;
	int rssi_threshold;
	unsigned int rann_interval;
	unsigned int preq_min_interval;
	unsigned int active_path_timeout;
	unsigned int max_peer_links;
	unsigned int max_preq_retries;
	bool tuned;
};

struct mesh_ap_definition {
	char section[64];
	char ifname[16];
};

enum mesh_mrp_profile {
	MESH_MRP_UNSPECIFIED,
	MESH_MRP_IPV4_ONLY,
	MESH_MRP_ULA_RA_UNIMPLEMENTED,
	MESH_MRP_GUA_UNIMPLEMENTED
};

struct mesh_mrp_config {
	enum mesh_mrp_profile profile;
	char lan_zone[32];
	char wan_zone[32];
	char uplink[16];
};

struct mesh_native_config {
	struct mesh_config config;
	struct mesh_mrp_config mrp;
	char group[129];
	char on_air_id[33];
	char phy[16];
	char mesh_ifname[16];
	char bridge_ifname[16];
	char network[32];
	char radio_section[32];
	char htmode[16];
	char country[3];
	char wan_device[16];
	/* Secret: memory only, never serialize except directly to trusted wpad
	 * configuration transport. Diagnostics and journals MUST NOT include it. */
	char credential[65];
	struct mesh_mobility mobility;
	struct mesh_file_stamp stamps[MESH_INPUT_FILES];
	size_t stamp_count;
	bool enabled;
	unsigned int portal_detect;
	unsigned int radio_channel;
	unsigned int radio_beacon_interval;
	unsigned int configured_ap_count;
	struct mesh_ap_definition aps[MESH_APS_MAX];
	bool beacon_workaround;
	bool peer_services_ready;
	bool manage_services;
	bool hold_mesh;
	bool dns_suppression_needed;
	bool odhcpd_ignored;
	bool odhcpd_maindhcp;
	char dns_instances[MESH_DNS_OWNERS_MAX][64];
	size_t dns_instance_count;
};

struct mesh_discovery_ops {
	void *context;
	int (*permanent_mac)(void *context, const char *name, uint8_t mac[6]);
	enum mesh_error (*radio)(void *context, uint32_t index, uint32_t frequency,
							struct mesh_radio *radio);
	enum mesh_error (*radio_config)(void *context, const struct mesh_native_config *config,
									struct mesh_radio *radio);
	enum mesh_error (*bridge)(void *context, const char *name, uint8_t mac[6]);
	enum mesh_error (*role)(void *context, const char *wan, enum mesh_role *role);
};

void mesh_sensitive_clear(void *memory, size_t length);
ssize_t mesh_readlink_at_root(int root_fd, const char *path, char *buffer, size_t capacity);
int mesh_open_at_root(int root_fd, const char *path, int flags);
int mesh_read_bounded_at(int root_fd, const char *path, void *buffer, size_t capacity,
						size_t *length, struct mesh_file_stamp *stamp);
int mesh_stamp_unchanged(int root_fd, const struct mesh_file_stamp *stamp);
/* Reads committed file snapshots via libuci import; never loads saved deltas. */
enum mesh_error mesh_native_config_read(int root_fd, struct mesh_native_config *output);
void mesh_native_config_clear(struct mesh_native_config *config);
enum mesh_error mesh_factory_collect(int root_fd, const char *wan,
									 const struct mesh_discovery_ops *ops,
									 struct mesh_factory_input *output);
enum mesh_error mesh_native_plan(int root_fd, const struct mesh_discovery_ops *ops,
								 struct mesh_native_config *config,
								 struct mesh_facts *facts, struct mesh_plan *plan);
enum mesh_error mesh_mobility_read(unsigned int level, bool portal,
								  bool beacon_workaround, struct mesh_mobility *out);

#endif
