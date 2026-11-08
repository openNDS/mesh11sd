/**
 * @file mesh_config.h
 * @brief Pure-mesh planner types and the plan and portal selection interface.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_RADIOS_MAX 8
#define MESH_PORTALS_MAX 32
#define MESH_HOLD_SECONDS 60
#define MESH_FAILURE_LIMIT 3

/* No password, key or PSK is stored in this API. The platform owns the secret. */
enum mesh_security {
	MESH_SECURITY_SAE = 1,
	MESH_SECURITY_EXPLICIT_OPEN = 2
};

enum mesh_role {
	MESH_ROLE_PEER,
	MESH_ROLE_MRP,
	MESH_ROLE_MBP
};

enum mesh_error {
	MESH_OK,
	MESH_E_IDENTITY,
	MESH_E_ADDRESS,
	MESH_E_RADIO,
	MESH_E_AMBIGUOUS_RADIO,
	MESH_E_SECURITY,
	MESH_E_POOL,
	MESH_E_INPUT,
	MESH_E_PLATFORM,
	MESH_E_ROLE_PENDING
};

struct mesh_identity {
	uint8_t factory_mac[6];
	uint8_t bridge_mac[6];
	char ula_prefix[48];
	char lan_hint[5];
	char vxlan_hint[5];
	char lan_prefix[48];
	char portal_address[48];
	char management_address[64];
};

#define MESH_COMBINATIONS_MAX 16
#define MESH_LIMITS_MAX 8

struct mesh_iflimit {
	uint64_t types;
	uint32_t maximum;
};

struct mesh_combination {
	struct mesh_iflimit limits[MESH_LIMITS_MAX];
	size_t limit_count;
	uint32_t maximum;
	uint32_t channels;
	uint32_t beacon_min_gcd;
	bool infrastructure_beacon_match;
};

struct mesh_radio {
	char phy[16];
	bool mesh_supported;
	bool frequency_supported;
	bool frequency_disabled;
	bool no_initiate_radiation;
	bool radar_required;
	bool multiple_hw_radios;
	/* A native discovery adapter must establish interface-combination safety. */
	bool combination_verified;
	struct mesh_combination combinations[MESH_COMBINATIONS_MAX];
	size_t combination_count;
};

struct mesh_facts {
	uint8_t factory_mac[6];
	uint8_t bridge_mac[6];
	struct mesh_radio radios[MESH_RADIOS_MAX];
	size_t radio_count;
	enum mesh_role detected_role;
};

struct mesh_ipv4 {
	/* Host byte order. All portals must be given the SAME group pool. */
	uint32_t address;
	uint8_t prefix;
	uint32_t pool_first;
	uint32_t pool_last;
	uint32_t lease_seconds;
};

struct mesh_config {
	const char *auto_mesh_id;
	const char *mesh_id;                 /* Explicit on-air ID, 1..32 bytes. */
	const char *preferred_phy;           /* NULL: only if exactly one usable radio. */
	const char *mesh_ifname;
	const char *bridge_ifname;
	uint32_t frequency_mhz;
	enum mesh_security security;
	bool credential_available;           /* Secret remains with platform/wpad. */
	uint64_t credential_revision;        /* Non-secret change token, not a key hash. */
	const char *ula_prefix_override;      /* Optional canonical ULA /48. */
	const char *lan_hint_override;        /* Optional four-hex-digit subnet. */
	const char *vxlan_hint_override;
	struct mesh_ipv4 portal_ipv4;
};

struct mesh_plan {
	struct mesh_identity identity;
	char phy[16];
	char mesh_ifname[16];
	char bridge_ifname[16];
	char mesh_id[33];
	uint32_t frequency_mhz;
	enum mesh_security security;
	uint64_t credential_revision;
	enum mesh_role role;
	struct mesh_ipv4 ipv4;
	bool dhcp_client;
	bool dhcp_server;
	bool dhcp_authoritative;
	bool dhcp_ping;
	bool dhcp_sequential;
	bool dhcp_arp_check;
	bool bridge_stp;
	bool loop_protection_active;
};

enum mesh_delta {
	MESH_DELTA_IDENTITY = 1,
	MESH_DELTA_RADIO = 2,
	MESH_DELTA_NETWORK = 4,
	MESH_DELTA_DHCP = 8
};

bool mesh_mac_valid(const uint8_t mac[6]);
int mesh_mac_parse(const char *text, uint8_t mac[6]);
enum mesh_error mesh_identity_build(const char *group, const uint8_t factory[6],
								   const uint8_t bridge[6], struct mesh_identity *out);
enum mesh_error mesh_plan_build(const struct mesh_config *config,
							   const struct mesh_facts *facts, struct mesh_plan *out);
unsigned int mesh_plan_delta(const struct mesh_plan *old, const struct mesh_plan *next);
const char *mesh_error_string(enum mesh_error error);
/* Deliberately excludes all caller-controlled strings and credential references. */
int mesh_plan_summary(const struct mesh_plan *plan, char *buffer, size_t length);

struct mesh_portal {
	uint8_t id[16];                     /* Binary IPv6 portal identity, not role. */
	uint64_t metric;                    /* Comparable complete-path metric from discovery. */
	bool reachable;                     /* Completed fresh controlled-path probe. */
	bool metric_unavailable;            /* Never use missing evidence as a zero/best cost. */
	enum mesh_role role;
};

struct mesh_portal_state {
	bool selected;
	uint8_t selected_id[16];
	uint64_t metric;
	uint64_t last_switch_seconds;
	bool challenging;
	uint8_t challenger_id[16];
	uint64_t challenger_since_seconds;
	uint64_t last_observation_seconds;
	bool observed;
	unsigned int failed_probes;
	bool mbp_backhaul_latched_off;
};

struct mesh_portal_result {
	bool identity_changed;
	bool renew_netifd_client;
	bool disable_backhaul;
};

/* Call once per completed probe round, using monotonic whole seconds.
 * Restart clears the state, including the MBP conflict latch. */
enum mesh_error mesh_portal_update(struct mesh_portal_state *state,
								  const struct mesh_portal *portals, size_t count,
								  enum mesh_role local_role, const uint8_t local_id[16],
								  uint64_t now, struct mesh_portal_result *result);

#endif
