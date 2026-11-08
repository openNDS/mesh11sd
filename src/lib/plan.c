/**
 * @file plan.c
 * @brief Build a deterministic pure-mesh plan from validated evidence.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mesh_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool name_valid(const char *text, size_t limit)
{
	size_t i;
	size_t length;

	if (text == NULL) {
		return false;
	}
	length = strlen(text);
	if (length == 0 || length >= limit) {
		return false;
	}
	for (i = 0; i < length; i++) {
		if (!isalnum((unsigned char)text[i]) && text[i] != '_' &&
			text[i] != '-' && text[i] != '.') {
			return false;
		}
	}
	return strcmp(text, ".") != 0 && strcmp(text, "..") != 0;
}

static bool hint_valid(const char *hint)
{
	size_t i;

	if (hint == NULL || strlen(hint) != 4) {
		return false;
	}
	for (i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)hint[i])) {
			return false;
		}
	}
	return true;
}

static void hint_copy(char out[5], const char *hint)
{
	size_t i;

	for (i = 0; i < 4; i++) {
		out[i] = (char)tolower((unsigned char)hint[i]);
	}
	out[4] = '\0';
}

static enum mesh_error overrides(const struct mesh_config *config,
								 struct mesh_identity *identity)
{
	char prefix[48];
	char base[32];
	char canonical[INET6_ADDRSTRLEN];
	struct in6_addr address;
	size_t length;
	size_t i;
	const uint8_t *bridge;

	if (config->ula_prefix_override == NULL && config->lan_hint_override == NULL &&
		config->vxlan_hint_override == NULL) {
		return MESH_OK;
	}
	if (config->ula_prefix_override != NULL) {
		length = strlen(config->ula_prefix_override);
		if (length < 5 || length >= sizeof(prefix) ||
			strcmp(config->ula_prefix_override + length - 3, "/48") != 0) {
			return MESH_E_ADDRESS;
		}
		memcpy(prefix, config->ula_prefix_override, length - 3);
		prefix[length - 3] = '\0';
	} else {
		length = strlen(identity->ula_prefix);
		memcpy(prefix, identity->ula_prefix, length - 3);
		prefix[length - 3] = '\0';
	}
	if (inet_pton(AF_INET6, prefix, &address) != 1 || address.s6_addr[0] != 0xfd) {
		return MESH_E_ADDRESS;
	}
	for (i = 6; i < 16; i++) {
		if (address.s6_addr[i] != 0) {
			return MESH_E_ADDRESS;
		}
	}
	if (config->lan_hint_override != NULL) {
		if (!hint_valid(config->lan_hint_override)) {
			return MESH_E_ADDRESS;
		}
		hint_copy(identity->lan_hint, config->lan_hint_override);
	}
	if (config->vxlan_hint_override != NULL) {
		if (!hint_valid(config->vxlan_hint_override)) {
			return MESH_E_ADDRESS;
		}
		hint_copy(identity->vxlan_hint, config->vxlan_hint_override);
	}
	snprintf(base, sizeof(base), "%02x%02x:%02x%02x:%02x%02x",
			 address.s6_addr[0], address.s6_addr[1], address.s6_addr[2],
			 address.s6_addr[3], address.s6_addr[4], address.s6_addr[5]);
	if (inet_ntop(AF_INET6, &address, canonical, sizeof(canonical)) == NULL) {
		return MESH_E_ADDRESS;
	}
	length = strlen(canonical);
	if (length + 4 > sizeof(identity->ula_prefix)) {
		return MESH_E_ADDRESS;
	}
	memset(identity->ula_prefix, 0, sizeof(identity->ula_prefix));
	memcpy(identity->ula_prefix, canonical, length);
	memcpy(identity->ula_prefix + length, "/48", 4);
	snprintf(identity->lan_prefix, sizeof(identity->lan_prefix), "%s:%s::/64",
			 base, identity->lan_hint);
	snprintf(identity->portal_address, sizeof(identity->portal_address), "%s:%s::1",
			 base, identity->lan_hint);
	bridge = identity->bridge_mac;
	snprintf(identity->management_address, sizeof(identity->management_address),
			 "%s:%s:%02x%02x:%02xff:fe%02x:%02x%02x/64", base, identity->lan_hint,
			 bridge[0], bridge[1], bridge[2], bridge[3], bridge[4], bridge[5]);
	return MESH_OK;
}

static bool pool_valid(const struct mesh_ipv4 *pool)
{
	uint32_t mask;
	uint32_t network;
	uint32_t broadcast;

	if (pool->prefix < 1 || pool->prefix > 30 || pool->lease_seconds == 0) {
		return false;
	}
	mask = UINT32_MAX << (32 - pool->prefix);
	network = pool->address & mask;
	broadcast = network | ~mask;
	if (pool->address <= network || pool->address >= broadcast ||
		pool->pool_first <= network || pool->pool_last >= broadcast ||
		pool->pool_first > pool->pool_last ||
		(pool->address >= pool->pool_first && pool->address <= pool->pool_last)) {
		return false;
	}
	return true;
}

static bool radio_usable(const struct mesh_radio *radio)
{
	/* DFS/NO_IR need regulatory/CAC lifecycle work, not a guessed success. */
	return name_valid(radio->phy, sizeof(radio->phy)) && radio->mesh_supported &&
		   radio->frequency_supported && !radio->frequency_disabled &&
		   !radio->no_initiate_radiation && !radio->radar_required &&
		   radio->combination_verified;
}

enum mesh_error mesh_plan_build(const struct mesh_config *config,
							   const struct mesh_facts *facts, struct mesh_plan *out)
{
	struct mesh_plan plan;
	const struct mesh_radio *selected;
	const char *mesh_ifname;
	const char *bridge_ifname;
	size_t i;
	size_t eligible;
	size_t length;
	enum mesh_error error;

	if (config == NULL || facts == NULL || out == NULL ||
		facts->radio_count > MESH_RADIOS_MAX ||
		facts->detected_role < MESH_ROLE_PEER || facts->detected_role > MESH_ROLE_MBP) {
		return MESH_E_INPUT;
	}
	/* The MBP conflict state machine is implemented separately. Its complete
	 * upstream/LAN runtime wiring must be recovered before planning that role. */
	if (facts->detected_role == MESH_ROLE_MBP) {
		return MESH_E_ROLE_PENDING;
	}
	/* Clear the entire object: deterministic output, no uninitialised padding. */
	memset(&plan, 0, sizeof(plan));
	error = mesh_identity_build(config->auto_mesh_id, facts->factory_mac,
								facts->bridge_mac, &plan.identity);
	if (error != MESH_OK) {
		return error;
	}
	error = overrides(config, &plan.identity);
	if (error != MESH_OK) {
		return error;
	}
	if (strcmp(plan.identity.lan_hint, plan.identity.vxlan_hint) == 0) {
		return MESH_E_ADDRESS;
	}
	if (config->mesh_id == NULL || config->mesh_id[0] == '\0' ||
		strlen(config->mesh_id) > 32 || config->frequency_mhz == 0) {
		return MESH_E_INPUT;
	}
	if (config->security != MESH_SECURITY_SAE &&
		config->security != MESH_SECURITY_EXPLICIT_OPEN) {
		return MESH_E_SECURITY;
	}
	if (config->security == MESH_SECURITY_SAE && !config->credential_available) {
		return MESH_E_SECURITY;
	}
	mesh_ifname = config->mesh_ifname != NULL ? config->mesh_ifname : "m11s0";
	bridge_ifname = config->bridge_ifname != NULL ? config->bridge_ifname : "br-lan";
	if (!name_valid(mesh_ifname, sizeof(plan.mesh_ifname)) ||
		!name_valid(bridge_ifname, sizeof(plan.bridge_ifname)) ||
		strcmp(mesh_ifname, bridge_ifname) == 0 ||
		(config->preferred_phy != NULL && !name_valid(config->preferred_phy, 16))) {
		return MESH_E_INPUT;
	}
	selected = NULL;
	eligible = 0;
	for (i = 0; i < facts->radio_count; i++) {
		if (!radio_usable(&facts->radios[i])) {
			continue;
		}
		if (config->preferred_phy != NULL &&
			strcmp(config->preferred_phy, facts->radios[i].phy) != 0) {
			continue;
		}
		selected = &facts->radios[i];
		eligible++;
	}
	if (eligible == 0) {
		return MESH_E_RADIO;
	}
	if (eligible != 1) {
		return MESH_E_AMBIGUOUS_RADIO;
	}
	memcpy(plan.phy, selected->phy, strlen(selected->phy) + 1);
	memcpy(plan.mesh_ifname, mesh_ifname, strlen(mesh_ifname) + 1);
	memcpy(plan.bridge_ifname, bridge_ifname, strlen(bridge_ifname) + 1);
	length = strlen(config->mesh_id);
	memcpy(plan.mesh_id, config->mesh_id, length + 1);
	plan.frequency_mhz = config->frequency_mhz;
	plan.security = config->security;
	plan.credential_revision = config->credential_revision;
	plan.role = facts->detected_role;
	plan.dhcp_client = plan.role == MESH_ROLE_PEER;
	plan.dhcp_server = plan.role == MESH_ROLE_MRP;
	if (plan.dhcp_server) {
		if (!pool_valid(&config->portal_ipv4)) {
			return MESH_E_POOL;
		}
		plan.ipv4.address = config->portal_ipv4.address;
		plan.ipv4.prefix = config->portal_ipv4.prefix;
		plan.ipv4.pool_first = config->portal_ipv4.pool_first;
		plan.ipv4.pool_last = config->portal_ipv4.pool_last;
		plan.ipv4.lease_seconds = config->portal_ipv4.lease_seconds;
	}
	plan.dhcp_authoritative = plan.dhcp_server;
	plan.dhcp_ping = plan.dhcp_server;
	plan.dhcp_sequential = false;
	plan.dhcp_arp_check = false;
	plan.bridge_stp = false;
	plan.loop_protection_active = false;
	*out = plan;
	return MESH_OK;
}

static bool ipv4_equal(const struct mesh_ipv4 *a, const struct mesh_ipv4 *b)
{
	return a->address == b->address && a->prefix == b->prefix &&
		   a->pool_first == b->pool_first && a->pool_last == b->pool_last &&
		   a->lease_seconds == b->lease_seconds;
}

unsigned int mesh_plan_delta(const struct mesh_plan *old, const struct mesh_plan *next)
{
	unsigned int delta;

	if (next == NULL) {
		return 0;
	}
	if (old == NULL) {
		return MESH_DELTA_IDENTITY | MESH_DELTA_RADIO | MESH_DELTA_NETWORK | MESH_DELTA_DHCP;
	}
	delta = 0;
	if (memcmp(&old->identity, &next->identity, sizeof(old->identity)) != 0) {
		delta |= MESH_DELTA_IDENTITY | MESH_DELTA_NETWORK;
	}
	if (strcmp(old->phy, next->phy) != 0 ||
		strcmp(old->mesh_ifname, next->mesh_ifname) != 0 ||
		strcmp(old->mesh_id, next->mesh_id) != 0 ||
		old->frequency_mhz != next->frequency_mhz || old->security != next->security ||
		old->credential_revision != next->credential_revision) {
		delta |= MESH_DELTA_RADIO;
	}
	if (strcmp(old->bridge_ifname, next->bridge_ifname) != 0 || old->role != next->role ||
		old->bridge_stp != next->bridge_stp ||
		old->loop_protection_active != next->loop_protection_active) {
		delta |= MESH_DELTA_NETWORK;
	}
	if (!ipv4_equal(&old->ipv4, &next->ipv4) ||
		old->dhcp_client != next->dhcp_client || old->dhcp_server != next->dhcp_server ||
		old->dhcp_authoritative != next->dhcp_authoritative ||
		old->dhcp_ping != next->dhcp_ping || old->dhcp_sequential != next->dhcp_sequential ||
		old->dhcp_arp_check != next->dhcp_arp_check) {
		delta |= MESH_DELTA_DHCP | MESH_DELTA_NETWORK;
	}
	return delta;
}

const char *mesh_error_string(enum mesh_error error)
{
	switch (error) {
	case MESH_OK:
		return "ok";
	case MESH_E_IDENTITY:
		return "missing or invalid mesh identity";
	case MESH_E_ADDRESS:
		return "invalid or conflicting IPv6 prefix/subnet override";
	case MESH_E_RADIO:
		return "no verified compatible radio for requested frequency";
	case MESH_E_AMBIGUOUS_RADIO:
		return "multiple compatible radios; explicit selection required";
	case MESH_E_SECURITY:
		return "explicit security and available platform credential required";
	case MESH_E_POOL:
		return "valid common IPv4 pool required for routed portal";
	case MESH_E_INPUT:
		return "invalid or incomplete configuration/observation";
	case MESH_E_PLATFORM:
		return "native platform discovery failed";
	case MESH_E_ROLE_PENDING:
		return "bridged-portal network planning not implemented yet";
	}
	return "unknown error";
}

int mesh_plan_summary(const struct mesh_plan *plan, char *buffer, size_t length)
{
	int written;

	if (plan == NULL || buffer == NULL || length == 0) {
		return -1;
	}
	written = snprintf(buffer, length,
					   "{\"mode\":\"plan-only\",\"role\":%u,\"frequency_mhz\":%u,"
					   "\"dhcp_client\":%s,\"dhcp_server\":%s,"
					   "\"loop_protection_active\":%s,\"applied\":false}",
					   (unsigned int)plan->role, plan->frequency_mhz,
					   plan->dhcp_client ? "true" : "false",
					   plan->dhcp_server ? "true" : "false",
					   plan->loop_protection_active ? "true" : "false");
	if (written < 0 || (size_t)written >= length) {
		buffer[0] = '\0';
		return -1;
	}
	return written;
}
