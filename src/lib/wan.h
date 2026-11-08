/**
 * @file wan.h
 * @brief WAN qualification result and configured-DNS health types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_WAN_H
#define MESH_WAN_H
#include "peer.h"
#define MESH_WAN_DNS_MAX 4
enum mesh_wan_health {
	MESH_WAN_ABSENT,
	MESH_WAN_PARTIAL,
	MESH_WAN_UNREACHABLE,
	MESH_WAN_QUALIFIED,
	MESH_WAN_ERROR
};
struct mesh_wan_evidence {
	char device[16];
	uint32_t source;
	uint8_t prefix;
	uint32_t dns[MESH_WAN_DNS_MAX];
	size_t dns_count;
};
struct mesh_wan_result {
	enum mesh_wan_health health;
	struct mesh_wan_evidence evidence;
};
/* Callback proves an actually assigned source, isolated device, kernel route
 * and correlated configured-DNS response. Zero qualified, positive unavailable,
 * negative malformed/system evidence. Host fixtures never call native I/O. */
struct mesh_wan_io {
	void *context;
	int (*probe)(void *, const struct mesh_wan_evidence *, uint32_t);
};
int mesh_wan_assess(int, struct mesh_peer *, const struct mesh_native_config *,
					const struct mesh_wan_io *, struct mesh_wan_result *);
int mesh_wan_status_decode(const struct mesh_runtime_data *, const struct mesh_native_config *,
						   const char *, struct mesh_wan_result *);
const char *mesh_wan_health_name(enum mesh_wan_health);
/* Finite beta auto-at-boot policy, not invented WAN hysteresis. */
int mesh_wan_boot_role(enum mesh_wan_health, bool, bool, enum mesh_role *);
int mesh_wan_configured_role(unsigned int, enum mesh_role);
int mesh_wan_role_guard(enum mesh_role, enum mesh_wan_health);
int mesh_wan_native_probe(void *, const struct mesh_wan_evidence *, uint32_t);
int mesh_wan_route_request(void *, size_t, size_t *, uint32_t, uint32_t, uint32_t);
int mesh_wan_route_decode(const void *, size_t, uint32_t, uint32_t, uint32_t);
int mesh_wan_dns_request(void *, size_t, size_t *, uint16_t);
int mesh_wan_dns_response(const void *, size_t, uint16_t);
#endif
