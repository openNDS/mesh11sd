/**
 * @file services.h
 * @brief Role-service owner, DNS include, and mode-journal types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_SERVICES_H
#define MESH_SERVICES_H

#include "peer.h"

#define MESH_SERVICE_MODES 4
#define MESH_SERVICE_PATH 256
#define MESH_SERVICE_MAGIC UINT32_C(0x4d385356)

struct mesh_dns_record {
	char instance[64];
	char config[MESH_SERVICE_PATH];
	char directory[MESH_SERVICE_PATH];
};

/* Only finite public mode enums, names and paths, NEVER configuration blobs,
 * credentials or private content hashes. Runtime journal, private 0600. */
struct mesh_services_disk {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t phase;
	char owner[33];
	char network[32];
	char bridge[16];
	uint8_t modes[MESH_SERVICE_MODES];
	uint8_t was_up;
	uint8_t dns_count;
	uint8_t dns_state; /* 0 suppressed,1 portal intent,2 portal active,3 suppress intent */
	uint8_t reserved[5];
	struct mesh_ipv4 portal_pool;
	struct mesh_dns_record dns[MESH_DNS_OWNERS_MAX];
};

struct mesh_services {
	struct mesh_peer *peer;
	int root;
	struct mesh_services_disk disk;
	struct mesh_file_stamp journal_stamp;
	struct mesh_file_stamp config_stamps[MESH_DNS_OWNERS_MAX];
	struct mesh_runtime_data dns_snapshot;
	bool journal_exists;
	bool recovering;
};

extern const char *const mesh_service_modes[MESH_SERVICE_MODES];
int mesh_services_journal(struct mesh_services *services);
int mesh_services_load(struct mesh_services *services);
int mesh_services_remove_journal(struct mesh_services *services);
int mesh_services_dns_discover(struct mesh_services *services, const struct mesh_native_config *config);
int mesh_services_dns_apply(struct mesh_services *services, bool suppress);
int mesh_services_dns_verify(struct mesh_services *services);
uint32_t mesh_services_dns_pid(struct mesh_services *services);
int mesh_services_dns_guard(struct mesh_services *services);
int mesh_services_open(struct mesh_peer *peer, int root, struct mesh_native_config *config);
void mesh_services_close(struct mesh_peer *peer);
int mesh_services_prepare(struct mesh_peer *peer);
int mesh_services_activate(struct mesh_peer *peer);
int mesh_services_down(struct mesh_peer *peer);
int mesh_services_restore(struct mesh_peer *peer);
int mesh_services_verify(struct mesh_peer *peer);
enum mesh_runtime_result mesh_peer_recover_managed(struct mesh_peer *peer, int root,
													struct mesh_native_config *config,
													const struct mesh_plan *plan);
enum mesh_runtime_result mesh_peer_managed_apply(struct mesh_peer *peer, int root,
												  struct mesh_native_config *config,
												  const struct mesh_plan *plan,
												  volatile sig_atomic_t *cancelled,
												  struct mesh_runtime_transaction *transaction);
enum mesh_runtime_result mesh_peer_managed_stop(struct mesh_peer *peer, int root,
												 struct mesh_native_config *config,
												 const struct mesh_plan *plan,
												 struct mesh_runtime_transaction *transaction);
#endif
