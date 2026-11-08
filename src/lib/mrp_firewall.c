/**
 * @file mrp_firewall.c
 * @brief Build the owned fw4 forward, masquerade, and input records.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mrp.h"
#include <stdio.h>
#include <string.h>

static void address(uint32_t value, char output[32])
{
	snprintf(output, 32, "%u.%u.%u.%u", value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

static bool string(struct blob_attr *table, const char *name, const char *expected)
{
	struct blob_attr *field = mesh_field(table, name, BLOBMSG_TYPE_STRING);
	return field != NULL && strcmp(blobmsg_get_string(field), expected) == 0;
}

static bool comparison(struct blob_attr *expression, const char *kind, const char *key,
						const char *field, const char *right, uint32_t number, bool numeric)
{
	struct blob_attr *match = mesh_field(expression, "match", BLOBMSG_TYPE_TABLE);
	struct blob_attr *left = mesh_field(match, "left", BLOBMSG_TYPE_TABLE);
	struct blob_attr *descriptor = mesh_field(left, kind, BLOBMSG_TYPE_TABLE);
	struct blob_attr *value = mesh_field(match, "right", numeric ? BLOBMSG_TYPE_INT32 : BLOBMSG_TYPE_STRING);
	return string(match, "op", "==") && string(descriptor, key, field) && value != NULL &&
		   (numeric ? blobmsg_get_u32(value) == number : strcmp(blobmsg_get_string(value), right) == 0);
}

static bool source_prefix(struct blob_attr *expression, const struct mesh_plan *plan)
{
	struct blob_attr *match = mesh_field(expression, "match", BLOBMSG_TYPE_TABLE);
	struct blob_attr *left = mesh_field(match, "left", BLOBMSG_TYPE_TABLE);
	struct blob_attr *payload = mesh_field(left, "payload", BLOBMSG_TYPE_TABLE);
	struct blob_attr *right = mesh_field(match, "right", BLOBMSG_TYPE_TABLE);
	struct blob_attr *prefix = mesh_field(right, "prefix", BLOBMSG_TYPE_TABLE);
	struct blob_attr *bits = mesh_field(prefix, "len", BLOBMSG_TYPE_INT32);
	char subnet[32];
	address(plan->ipv4.address & (UINT32_MAX << (32 - plan->ipv4.prefix)), subnet);
	return string(match, "op", "==") && string(payload, "protocol", "ip") && string(payload, "field", "saddr") &&
		   string(prefix, "addr", subnet) && bits != NULL && blobmsg_get_u32(bits) == plan->ipv4.prefix;
}

static int rule_valid(struct blob_attr *rule, unsigned int index, struct mesh_peer *peer, unsigned int *protocol)
{
	const struct mesh_native_config *config = peer->config;
	struct blob_attr *expressions = mesh_field(rule, "expr", BLOBMSG_TYPE_ARRAY);
	struct blob_attr *expression;
	struct blob_attr *jump;
	int left;
	bool device = false;
	bool ipv4 = index < 2;
	bool source = index >= 2;
	bool port = index < 2;
	bool terminal = false;
	bool tcp = false;
	bool udp = false;
	char target[64];
	if (expressions == NULL) {
		return -1;
	}
	snprintf(target, sizeof(target), "accept_to_%s", config->mrp.wan_zone);
	blobmsg_for_each_attr(expression, expressions, left) {
		struct blob_attr *item;
		struct blob_attr *match;
		struct blob_attr *lhs;
		struct blob_attr *payload;
		size_t fields = 0;
		int remaining;
		bool device_match;
		bool source_match;
		bool port_match;
		bool verdict;
		bool transport;
		bool family;
		if (terminal) {
			return -1;
		}
		blobmsg_for_each_attr(item, expression, remaining) {
			fields++;
		}
		if (fields != 1) {
			return -1;
		}
		device_match = comparison(expression, "meta", "key", index < 2 ? "oifname" : "iifname",
								   index < 2 ? config->mrp.uplink : config->bridge_ifname, 0, false);
		source_match = index < 2 && source_prefix(expression, peer->plan);
		port_match = index >= 2 && comparison(expression, "payload", "field", "dport", NULL,
											   index == 2 ? 53 : 67, true);
		transport = index >= 2 &&
					(comparison(expression, "meta", "key", "l4proto", "tcp", 0, false) ||
					 comparison(expression, "meta", "key", "l4proto", "udp", 0, false));
		family = comparison(expression, "meta", "key", "nfproto", "ipv4", 0, false);
		jump = mesh_field(expression, "jump", BLOBMSG_TYPE_TABLE);
		verdict = index == 0 ? string(jump, "target", target) :
				  mesh_field(expression, index == 1 ? "masquerade" : "accept", -1) != NULL;
		if (!device_match && !source_match && !port_match && !transport && !family && !verdict &&
			mesh_field(expression, "counter", BLOBMSG_TYPE_TABLE) == NULL) {
			return -1;
		}
		device = device || device_match;
		ipv4 = ipv4 || family;
		source = source || source_match;
		port = port || port_match;
		tcp = tcp || comparison(expression, "meta", "key", "l4proto", "tcp", 0, false);
		udp = udp || comparison(expression, "meta", "key", "l4proto", "udp", 0, false);
		match = mesh_field(expression, "match", BLOBMSG_TYPE_TABLE);
		lhs = mesh_field(match, "left", BLOBMSG_TYPE_TABLE);
		payload = mesh_field(lhs, "payload", BLOBMSG_TYPE_TABLE);
		tcp = tcp || string(payload, "protocol", "tcp");
		udp = udp || string(payload, "protocol", "udp");
		terminal = verdict;
	}
	*protocol = tcp ? 1U : udp ? 2U : 0U;
	return device && ipv4 && source && port && terminal && (index < 2 || (index == 2 ? tcp != udp : udp && !tcp)) ? 0 : -1;
}

int mesh_mrp_firewall_verify(struct mesh_peer *peer, bool absent)
{
	struct mesh_runtime_data reply = {0};
	struct blob_attr *entries;
	struct blob_attr *entry;
	struct blob_attr *rule;
	struct blob_attr *comment;
	char chain[64];
	char wanted[128];
	unsigned int index;
	unsigned int protocol;
	unsigned int seen;
	unsigned int count;
	int left;
	int result = -1;
	if (peer->config == NULL || peer->transport.firewall_chain == NULL || (!absent && (peer->plan == NULL ||
		peer->plan->ipv4.prefix == 0 || peer->plan->ipv4.prefix > 30))) {
		return -1;
	}
	for (index = 0; index < 4; index++) {
		snprintf(chain, sizeof(chain), "%s_%s", index == 0 ? "forward" : index == 1 ? "srcnat" : "input",
				 index == 1 ? peer->config->mrp.wan_zone : peer->config->mrp.lan_zone);
		if (peer->transport.firewall_chain(peer->transport.context, chain, &reply) != 0 || !mesh_reply_valid(&reply) ||
			(entries = mesh_root_field(&reply, "nftables", BLOBMSG_TYPE_ARRAY)) == NULL) {
			goto done;
		}
		snprintf(wanted, sizeof(wanted), "!fw4: ubus:%s[*] %s %u", MESH_PEER_SERVICE, index == 1 ? "nat" : "rule", index);
		count = 0;
		seen = 0;
		blobmsg_for_each_attr(entry, entries, left) {
			rule = mesh_field(entry, "rule", BLOBMSG_TYPE_TABLE);
			comment = mesh_field(rule, "comment", BLOBMSG_TYPE_STRING);
			if (comment == NULL || strcmp(blobmsg_get_string(comment), wanted) != 0) {
				continue;
			}
			count++;
			if (!string(rule, "family", "inet") || !string(rule, "table", "fw4") || !string(rule, "chain", chain) ||
				(!absent && rule_valid(rule, index, peer, &protocol) != 0)) {
				goto done;
			}
			if (!absent) {
				seen |= protocol;
			}
		}
		if (absent ? count != 0 : (count != (index == 2 ? 2U : 1U) || (index == 2 && seen != 3))) {
			goto done;
		}
	}
	result = 0;
done:
	mesh_runtime_data_clear(&reply);
	return result;
}

int mesh_mrp_verify(struct mesh_peer *peer)
{
	struct mesh_services *services = peer->services;
	const struct mesh_native_config *config = peer->config;
	const struct mesh_plan *plan = peer->plan;
	struct mesh_runtime_data reply = {0};
	struct mesh_message request;
	struct blob_attr *addresses;
	struct blob_attr *item;
	struct blob_attr *mask;
	struct blob_attr *field;
	char ip[32];
	int left;
	int result = -1;
	bool found = false;
	mesh_message_init(&request);
	if (services == NULL || config == NULL || plan == NULL || config->mrp.profile != MESH_MRP_IPV4_ONLY ||
		plan->role != MESH_ROLE_MRP || peer->transport.portal_probe == NULL || services->disk.dns_state != 2 ||
		mesh_services_verify(peer) != 0 || mesh_mrp_firewall_verify(peer, false) != 0 ||
		mesh_peer_call(peer, "network.interface.mesh11sd_v8_client", "status", &request, &reply) != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
	if (field == NULL || !blobmsg_get_bool(field)) {
		goto done;
	}
	field = mesh_root_field(&reply, "proto", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), "static") != 0) {
		goto done;
	}
	field = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), config->bridge_ifname) != 0) {
		goto done;
	}
	addresses = mesh_root_field(&reply, "ipv4-address", BLOBMSG_TYPE_ARRAY);
	if (addresses == NULL) {
		goto done;
	}
	address(plan->ipv4.address, ip);
	blobmsg_for_each_attr(item, addresses, left) {
		mask = mesh_field(item, "mask", BLOBMSG_TYPE_INT32);
		found = found || (string(item, "address", ip) && mask != NULL && blobmsg_get_u32(mask) == plan->ipv4.prefix);
	}
	if (found && peer->transport.portal_probe(peer->transport.context, config->bridge_ifname, config->mrp.uplink,
											  plan->ipv4.address, plan->ipv4.prefix, mesh_services_dns_pid(services)) == 0) {
		result = 0;
	}
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}
