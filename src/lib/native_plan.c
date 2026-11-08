/**
 * @file native_plan.c
 * @brief Connect committed inputs and kernel evidence to the planner.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "native.h"
#include "platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static bool real_root(int root)
{
	struct stat selected;
	struct stat actual;

	return fstat(root, &selected) == 0 && stat("/", &actual) == 0 &&
		   selected.st_dev == actual.st_dev && selected.st_ino == actual.st_ino;
}

enum mesh_error mesh_native_plan(int root_fd, const struct mesh_discovery_ops *ops,
								 struct mesh_native_config *config,
								 struct mesh_facts *facts, struct mesh_plan *plan)
{
	struct mesh_factory_input identity_input;
	struct mesh_factory_result identity;
	struct mesh_facts detected;
	struct mesh_plan result;
	unsigned long index;
	char *end;
	char path[128];
	char evidence[MESH_INPUT_MAX];
	size_t length;
	int written;
	size_t i;
	enum mesh_error error;

	if (config == NULL || facts == NULL || plan == NULL) {
		return MESH_E_INPUT;
	}
	if (!real_root(root_fd) && (ops == NULL || ops->bridge == NULL || (ops->radio == NULL && ops->radio_config == NULL) || ops->role == NULL)) {
		mesh_native_config_clear(config);
		return MESH_E_PLATFORM;
	}
	error = mesh_native_config_read(root_fd, config);
	if (error != MESH_OK) {
		mesh_native_config_clear(config);
		return error;
	}
	if (config->configured_ap_count != 0 && (ops == NULL || ops->radio_config == NULL)) {
		/* AP+mesh proof must include desired committed APs, not only currently
		 * running interfaces. Never silently disable those APs. */
		error = MESH_E_RADIO;
		goto failed;
	}
	if (!config->enabled) {
		error = MESH_E_INPUT;
		goto failed;
	}
	if (mesh_read_bounded_at(root_fd, "etc/board.json", evidence, sizeof(evidence), &length,
							 &config->stamps[config->stamp_count]) == 0) {
		config->stamp_count++;
	} else if (errno != ENOENT) {
		error = MESH_E_INPUT;
		goto failed;
	}
	memset(&detected, 0, sizeof(detected));
	error = mesh_factory_collect(root_fd, config->wan_device, ops, &identity_input);
	if (error != MESH_OK) {
		goto failed;
	}
	error = mesh11sd_factory_mac(&identity_input, &identity);
	if (error != MESH_OK) {
		goto failed;
	}
	memcpy(detected.factory_mac, identity.mac, 6);
	error = ops != NULL && ops->bridge != NULL ?
			ops->bridge(ops->context, config->bridge_ifname, detected.bridge_mac) :
			mesh_probe_bridge(config->bridge_ifname, detected.bridge_mac);
	if (error != MESH_OK) {
		goto failed;
	}
	written = snprintf(path, sizeof(path), "sys/class/net/%s/bridge/stp_state", config->bridge_ifname);
	if (written < 0 || (size_t)written >= sizeof(path) ||
		mesh_read_bounded_at(root_fd, path, evidence, sizeof(evidence), &length,
							 &config->stamps[config->stamp_count]) != 0 ||
		!((length == 2 && memcmp(evidence, "0\n", 2) == 0) ||
		  (length == 1 && evidence[0] == '0'))) {
		/* STP must already be disabled until reversible bridge-state apply
		 * exists. Never silently leave STP running on a mesh-used bridge. */
		error = MESH_E_PLATFORM;
		goto failed;
	}
	config->stamp_count++;
	index = strtoul(config->phy + 3, &end, 10);
	if (*end != '\0' || index > UINT32_MAX) {
		error = MESH_E_RADIO;
		goto failed;
	}
	error = ops != NULL && ops->radio_config != NULL ?
			ops->radio_config(ops->context, config, &detected.radios[0]) :
			ops != NULL && ops->radio != NULL ?
			ops->radio(ops->context, (uint32_t)index, config->config.frequency_mhz, &detected.radios[0]) :
			mesh_probe_empty_radio((uint32_t)index, config->config.frequency_mhz, &detected.radios[0]);
	if (error != MESH_OK || strcmp(config->phy, detected.radios[0].phy) != 0) {
		error = MESH_E_RADIO;
		goto failed;
	}
	detected.radio_count = 1;
	if (config->portal_detect == 0) {
		detected.detected_role = MESH_ROLE_MRP;
	} else if (config->portal_detect == 1 || config->portal_detect == 2) {
		error = ops != NULL && ops->role != NULL ?
				ops->role(ops->context, config->wan_device, &detected.detected_role) :
				mesh_probe_wan_role(config->wan_device, &detected.detected_role);
		if (error != MESH_OK) {
			goto failed;
		}
	} else {
		error = MESH_E_ROLE_PENDING;
		goto failed;
	}
	error = mesh_mobility_read(config->mobility.level, detected.detected_role == MESH_ROLE_MRP,
							   config->beacon_workaround || detected.radios[0].multiple_hw_radios, &config->mobility);
	if (error != MESH_OK) {
		goto failed;
	}
	error = mesh_plan_build(&config->config, &detected, &result);
	if (error != MESH_OK) {
		goto failed;
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(root_fd, &config->stamps[i]) != 0) {
			error = MESH_E_INPUT;
			goto failed;
		}
	}
	mesh_sensitive_clear(evidence, sizeof(evidence));
	*plan = result;
	*facts = detected;
	return MESH_OK;
failed:
	mesh_sensitive_clear(evidence, sizeof(evidence));
	mesh_native_config_clear(config);
	return error;
}
