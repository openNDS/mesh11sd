/**
 * @file peer.h
 * @brief Owned procd peer backend types and transport hooks.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_PEER_H
#define MESH_PEER_H

#include "runtime.h"
#include "platform.h"
#include <libubox/blobmsg.h>

#define MESH_PEER_SERVICE "mesh11sd-v8-runtime"
#define MESH_PEER_OWNER "mesh11sd-v8-owner"
#define MESH_PEER_CALL_UNCERTAIN (-2)

struct mesh_services;

struct mesh_peer_transport {
	void *context;
	int (*invoke)(void *context, const char *object, const char *method,
				  struct blob_attr *request, struct mesh_runtime_data *reply);
	uint64_t (*now_ms)(void *context);
	void (*wait_ms)(void *context, unsigned int milliseconds);
	enum mesh_error (*interfaces)(void *context, struct mesh_interface_list *output);
	enum mesh_error (*wiphy)(void *context, uint32_t index, uint32_t frequency, struct mesh_radio *radio);
	int (*restart_dnsmasq)(void *context, const char *instance);
	int (*reload_firewall)(void *context);
	int (*firewall_chain)(void *context, const char *chain, struct mesh_runtime_data *reply);
	int (*portal_probe)(void *context, const char *bridge, const char *uplink,
						uint32_t gateway, uint8_t prefix, uint32_t dns_pid);
	int (*verify_bridge)(void *context, const struct mesh_plan *plan);
};

struct mesh_peer {
	enum mesh_role confirmed_startup_role;
	bool startup_role_confirmed;
	struct mesh_peer_transport transport;
	struct mesh_services *services;
	struct mesh_runtime_ops ops;
	const struct mesh_native_config *config;
	const struct mesh_plan *plan;
	struct mesh_interface_list baseline;
	char owner[33];
	int session_lock;
	int directory;
	bool present;
	bool initialized;
	bool have_baseline;
	bool recovery_cleanup;
	volatile sig_atomic_t *cancelled;
};

struct mesh_message {
	struct blob_buf blob;
	unsigned char storage[MESH_RUNTIME_BYTES];
};

int mesh_message_init(struct mesh_message *message);
void mesh_message_clear(struct mesh_message *message);
bool mesh_reply_valid(const struct mesh_runtime_data *data);
struct blob_attr *mesh_field(struct blob_attr *table, const char *name, int type);
struct blob_attr *mesh_root_field(const struct mesh_runtime_data *data, const char *name, int type);
int mesh_message_copy(const struct mesh_message *message, struct mesh_runtime_data *output);

/* Holds a lifetime exclusive flock in an owned 0700 directory; owner ID is
 * noncredential metadata. No production transport is called by these tests. */
int mesh_peer_open(struct mesh_peer *peer, int directory, const struct mesh_peer_transport *transport);
void mesh_peer_close(struct mesh_peer *peer);
int mesh_peer_call(struct mesh_peer *peer, const char *object, const char *method,
				   struct mesh_message *request, struct mesh_runtime_data *reply);
int mesh_peer_preflight(struct mesh_peer *peer, const struct mesh_native_config *config);
enum mesh_error mesh_peer_radio(void *context, const struct mesh_native_config *config, struct mesh_radio *radio);
/* Crash recovery only adopts an unchanged, reconstructable owned payload.
 * Different/foreign data is left untouched. */
/* Recover an interrupted withdrawal: service gone, netifd still has old objects.
 * Requires recognized private metadata; only reloads committed configuration. */
enum mesh_runtime_result mesh_peer_recover_detached(struct mesh_peer *peer, int root,
													const struct mesh_native_config *config);
enum mesh_runtime_result mesh_peer_recover(struct mesh_peer *peer, int root,
										   const struct mesh_native_config *config,
										   const struct mesh_plan *plan,
										   const struct mesh_runtime_data *wanted);

struct mesh_live_bus;
struct mesh_live_bus *mesh_live_bus_open(bool permit_mutation);
void mesh_live_bus_close(struct mesh_live_bus *bus);
void mesh_live_transport(struct mesh_live_bus *bus, struct mesh_peer_transport *transport);
#endif
