/**
 * @file fixture.h
 * @brief Public synthetic planner inputs used by host checks.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_FIXTURE_H
#define MESH_FIXTURE_H

#include "mesh_config.h"
#include <string.h>

/* PUBLIC SYNTHETIC TEST INPUTS ONLY. Not a default deployment configuration.
 * An available-credential flag tests the API: it is NOT an embedded SAE key. */
static inline void mesh_test_fixture(struct mesh_config *config, struct mesh_facts *facts)
{
	memset(config, 0, sizeof(*config));
	memset(facts, 0, sizeof(*facts));
	config->auto_mesh_id = "--__";
	config->mesh_id = "TEST-ONLY-NOT-A-DEPLOYMENT";
	config->frequency_mhz = 2412;
	config->security = MESH_SECURITY_SAE;
	config->credential_available = true;
	config->credential_revision = 1;
	config->portal_ipv4.address = 0xc0000201U;
	config->portal_ipv4.prefix = 24;
	config->portal_ipv4.pool_first = 0xc0000264U;
	config->portal_ipv4.pool_last = 0xc00002feU;
	config->portal_ipv4.lease_seconds = 3600;
	(void)mesh_mac_parse("02:00:00:00:00:0a", facts->factory_mac);
	(void)mesh_mac_parse("02:11:22:33:44:55", facts->bridge_mac);
	facts->radio_count = 1;
	memcpy(facts->radios[0].phy, "phy0", 5);
	facts->radios[0].mesh_supported = true;
	facts->radios[0].frequency_supported = true;
	facts->radios[0].combination_verified = true;
	facts->detected_role = MESH_ROLE_PEER;
}

#endif
