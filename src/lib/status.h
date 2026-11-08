/**
 * @file status.h
 * @brief Fixed status schema, including the inactive-capability flags.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_STATUS_H
#define MESH_STATUS_H
#include "wan.h"

enum mesh_status_phase {
	MESH_STATUS_VALIDATING,
	MESH_STATUS_STARTING,
	MESH_STATUS_ACTIVE,
	MESH_STATUS_WITHDRAWING,
	MESH_STATUS_CONFLICT
};
struct mesh_public_status {
	enum mesh_status_phase phase;
	enum mesh_role role;
	enum mesh_wan_health wan_health;
	bool automatic_at_boot;
	bool wan_assessed;
	bool mesh_active;
	bool ipv4_services_active;
	bool portal_selected;
};
struct mesh_status_service;
int mesh_status_payload(const struct mesh_public_status *status, struct mesh_runtime_data *out);
int mesh_status_decode(const struct mesh_runtime_data *data, struct mesh_public_status *status);
struct mesh_status_service *mesh_status_open(const struct mesh_public_status *status, const char *socket_path);
int mesh_status_dispatch(struct mesh_status_service *service, unsigned int milliseconds);
void mesh_status_close(struct mesh_status_service *service);
int mesh_status_query(const char *socket_path, struct mesh_public_status *status);
int mesh_status_print(const struct mesh_public_status *status);
#endif
