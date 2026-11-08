/**
 * @file wan.c
 * @brief Read committed WAN device, prefix, route, and DNS evidence.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "wan.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <uci.h>
#include <unistd.h>

const char *mesh_wan_health_name(enum mesh_wan_health health)
{
	static const char *const names[] = {"absent", "partial", "unreachable", "configured_dns_qualified", "error"};
	return health >= MESH_WAN_ABSENT && health <= MESH_WAN_ERROR ? names[health] : "error";
}
int mesh_wan_boot_role(enum mesh_wan_health health, bool automatic, bool request_mrp, enum mesh_role *role)
{
	if (role == NULL || (automatic && request_mrp)) {
		return -1;
	}
	if (!automatic && !request_mrp) {
		*role = MESH_ROLE_PEER;
		return 0;
	}
	if (health < MESH_WAN_ABSENT || health >= MESH_WAN_ERROR ||
		(request_mrp && health != MESH_WAN_QUALIFIED)) {
		return -1;
	}
	*role = health == MESH_WAN_QUALIFIED ? MESH_ROLE_MRP : MESH_ROLE_PEER;
	return 0;
}
int mesh_wan_configured_role(unsigned int portal_detect, enum mesh_role role)
{
	/* Existing planner: 0 explicitly forces MRP; 1/2 use the role evidence.
	 * Do not override a committed forced portal with an automatic peer plan. */
	if (portal_detect > 2 || (role != MESH_ROLE_PEER && role != MESH_ROLE_MRP) ||
		(portal_detect == 0 && role != MESH_ROLE_MRP)) {
		return -1;
	}
	return 0;
}
int mesh_wan_role_guard(enum mesh_role role, enum mesh_wan_health health)
{
	if (role == MESH_ROLE_PEER) {
		return 0;
	}
	if (role != MESH_ROLE_MRP || health < MESH_WAN_ABSENT || health > MESH_WAN_ERROR) {
		return -1;
	}
	return health == MESH_WAN_QUALIFIED ? 0 : 1;
}
static bool name_valid(const char *name)
{
	size_t i;
	if (name == NULL || name[0] == '\0' || strlen(name) >= 16) {
		return false;
	}
	for (i = 0; name[i] != '\0'; i++) {
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
			  (name[i] >= '0' && name[i] <= '9') || strchr("_.:-", name[i]) != NULL)) {
			return false;
		}
	}
	return true;
}
static bool unicast(uint32_t address)
{
	return (address >> 24) != 0 && (address >> 24) != 127 && address < UINT32_C(0xe0000000) &&
		   (address & UINT32_C(0xffff0000)) != UINT32_C(0xa9fe0000);
}
static bool overlaps(uint32_t address, uint8_t prefix, const struct mesh_ipv4 *pool)
{
	uint8_t bits = prefix < pool->prefix ? prefix : pool->prefix;
	uint32_t mask = bits == 0 ? 0 : UINT32_MAX << (32 - bits);
	return (address & mask) == (pool->address & mask);
}
/* Parse the committed, stamped network file independently so corrective6a's
 * input adapter stays byte-identical. No temporary UCI state or commit. */
static int committed(int root, const struct mesh_native_config *config, char proto[16])
{
	struct uci_context *context = NULL;
	struct uci_package *package = NULL;
	struct uci_section *wan;
	struct mesh_file_stamp stamp;
	char raw[65536];
	const char *value;
	size_t size;
	size_t i;
	FILE *input = NULL;
	int result = -1;
	bool stamped = false;
	for (i = 0; i < config->stamp_count; i++) {
		if (strcmp(config->stamps[i].relative_path, "etc/config/network") == 0) {
			if (mesh_stamp_unchanged(root, &config->stamps[i]) != 0) {
				goto done;
			}
			stamped = true;
		}
	}
	if (!stamped || mesh_read_bounded_at(root,"etc/config/network",raw,sizeof(raw),&size,&stamp) != 0 ||
		(context = uci_alloc_context()) == NULL || (input = fmemopen(raw,size,"r")) == NULL ||
		uci_import(context,input,"network",&package,true) != UCI_OK) {
		goto done;
	}
	wan = uci_lookup_section(context,package,"wan");
	if (wan == NULL) {
		result = 1;
		goto done;
	}
	if (strcmp(wan->type,"interface") != 0) {
		goto done;
	}
	{
		static const char *const keys[] = {"disabled", "auto", "device", "proto"};
		for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
			struct uci_option *option = uci_lookup_option(context,wan,keys[i]);
			if (option != NULL && option->type != UCI_TYPE_STRING) {
				goto done;
			}
		}
	}
	value = uci_lookup_option_string(context,wan,"disabled");
	if (value != NULL && strcmp(value,"0") != 0 && strcmp(value,"1") != 0) {
		goto done;
	}
	if (value != NULL && strcmp(value,"1") == 0) {
		result = 1;
		goto done;
	}
	value = uci_lookup_option_string(context,wan,"auto");
	if (value != NULL && strcmp(value,"0") != 0 && strcmp(value,"1") != 0) {
		goto done;
	}
	if (value != NULL && strcmp(value,"0") == 0) {
		result = 1;
		goto done;
	}
	value = uci_lookup_option_string(context,wan,"device");
	if (!name_valid(value) || strcmp(value,config->wan_device) != 0 || strcmp(value,config->bridge_ifname) == 0) {
		result = 2;
		goto done;
	}
	value = uci_lookup_option_string(context,wan,"proto");
	if (value == NULL || (strcmp(value,"dhcp") != 0 && strcmp(value,"static") != 0)) {
		result = 2;
		goto done;
	}
	memcpy(proto,value,strlen(value)+1);
	result = 0;
done:
	if (input != NULL) {
		fclose(input);
	}
	if (context != NULL) {
		uci_free_context(context);
	}
	mesh_sensitive_clear(raw,sizeof(raw));
	if (stamped) {
		for (i = 0; i < config->stamp_count; i++) {
			if (strcmp(config->stamps[i].relative_path,"etc/config/network") == 0 &&
				mesh_stamp_unchanged(root,&config->stamps[i]) != 0) {
				result = -1;
			}
		}
	}
	return result;
}
int mesh_wan_status_decode(const struct mesh_runtime_data *reply, const struct mesh_native_config *config,
						   const char *proto, struct mesh_wan_result *out)
{
	struct mesh_wan_result result = { .health = MESH_WAN_PARTIAL };
	struct blob_attr *field;
	struct blob_attr *array;
	struct blob_attr *item;
	struct blob_attr *address;
	struct blob_attr *mask;
	struct in_addr parsed;
	bool default_route = false;
	int left;
	size_t i;
	if (reply == NULL || config == NULL || proto == NULL || out == NULL || !mesh_reply_valid(reply)) {
		return -1;
	}
	field = mesh_root_field(reply,"up",BLOBMSG_TYPE_BOOL);
	if (field == NULL) {
		return -1;
	}
	if (!blobmsg_get_bool(field)) {
		result.health = MESH_WAN_ABSENT;
		*out = result;
		return 0;
	}
	field = mesh_root_field(reply,"pending",BLOBMSG_TYPE_BOOL);
	if (field == NULL) {
		return -1;
	}
	if (blobmsg_get_bool(field)) {
		*out = result;
		return 0;
	}
	field = mesh_root_field(reply,"proto",BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field),proto) != 0) {
		*out = result;
		return 0;
	}
	field = mesh_root_field(reply,"device",BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field),config->wan_device) != 0) {
		*out = result;
		return 0;
	}
	field = mesh_root_field(reply,"l3_device",BLOBMSG_TYPE_STRING);
	if (field == NULL || !name_valid(blobmsg_get_string(field)) ||
		strcmp(blobmsg_get_string(field),config->wan_device) != 0 ||
		strcmp(blobmsg_get_string(field),config->bridge_ifname) == 0) {
		*out = result;
		return 0;
	}
	memcpy(result.evidence.device,config->wan_device,strlen(config->wan_device)+1);
	array = mesh_root_field(reply,"ipv4-address",BLOBMSG_TYPE_ARRAY);
	if (array == NULL) {
		*out = result;
		return 0;
	}
	blobmsg_for_each_attr(item,array,left) {
		address = mesh_field(item,"address",BLOBMSG_TYPE_STRING);
		mask = mesh_field(item,"mask",BLOBMSG_TYPE_INT32);
		if (address == NULL || mask == NULL || result.evidence.source != 0 ||
			inet_pton(AF_INET,blobmsg_get_string(address),&parsed) != 1 || blobmsg_get_u32(mask) == 0 ||
			blobmsg_get_u32(mask) > 32 || !unicast(ntohl(parsed.s_addr))) {
			return -1;
		}
		result.evidence.source = ntohl(parsed.s_addr);
		result.evidence.prefix = (uint8_t)blobmsg_get_u32(mask);
		if (overlaps(result.evidence.source,result.evidence.prefix,&config->config.portal_ipv4)) {
			*out = result;
			return 0;
		}
	}
	array = mesh_root_field(reply,"route",BLOBMSG_TYPE_ARRAY);
	if (array == NULL || result.evidence.source == 0) {
		*out = result;
		return 0;
	}
	blobmsg_for_each_attr(item,array,left) {
		address = mesh_field(item,"target",BLOBMSG_TYPE_STRING);
		mask = mesh_field(item,"mask",BLOBMSG_TYPE_INT32);
		if (address == NULL || mask == NULL) {
			return -1;
		}
		default_route = default_route || (strcmp(blobmsg_get_string(address),"0.0.0.0") == 0 && blobmsg_get_u32(mask) == 0);
	}
	array = mesh_root_field(reply,"dns-server",BLOBMSG_TYPE_ARRAY);
	if (!default_route || array == NULL) {
		*out = result;
		return 0;
	}
	blobmsg_for_each_attr(item,array,left) {
		uint32_t server;
		bool duplicate = false;
		if (blobmsg_type(item) != BLOBMSG_TYPE_STRING) {
			return -1;
		}
		if (inet_pton(AF_INET,blobmsg_get_string(item),&parsed) != 1) {
			struct in6_addr ipv6;
			if (inet_pton(AF_INET6,blobmsg_get_string(item),&ipv6) != 1) {
				return -1;
			}
			continue;
		}
		server = ntohl(parsed.s_addr);
		if (!unicast(server) || server == result.evidence.source || overlaps(server,32,&config->config.portal_ipv4)) {
			continue;
		}
		for (i = 0; i < result.evidence.dns_count; i++) {
			duplicate = duplicate || result.evidence.dns[i] == server;
		}
		if (!duplicate) {
			if (result.evidence.dns_count == MESH_WAN_DNS_MAX) {
				return -1;
			}
			result.evidence.dns[result.evidence.dns_count++] = server;
		}
	}
	if (result.evidence.dns_count != 0) {
		result.health = MESH_WAN_UNREACHABLE;
	}
	*out = result;
	return 0;
}
int mesh_wan_assess(int root, struct mesh_peer *peer, const struct mesh_native_config *config,
					const struct mesh_wan_io *io, struct mesh_wan_result *out)
{
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	char proto[16];
	int state;
	int tested;
	size_t i;
	if (peer == NULL || config == NULL || io == NULL || io->probe == NULL || out == NULL ||
		mesh_message_init(&request) != 0) {
		return -1;
	}
	memset(out,0,sizeof(*out));
	state = committed(root,config,proto);
	if (state != 0) {
		out->health = state < 0 ? MESH_WAN_ERROR : state == 1 ? MESH_WAN_ABSENT : MESH_WAN_PARTIAL;
	} else if (mesh_peer_call(peer,"network.interface.wan","status",&request,&reply) != 0 ||
			   mesh_wan_status_decode(&reply,config,proto,out) != 0) {
		out->health = MESH_WAN_ERROR;
	} else if (out->health == MESH_WAN_UNREACHABLE) {
		for (i = 0; i < out->evidence.dns_count; i++) {
			tested = io->probe(io->context,&out->evidence,out->evidence.dns[i]);
			if (tested == 0) {
				out->health = MESH_WAN_QUALIFIED;
				break;
			}
			if (tested < 0) {
				out->health = MESH_WAN_ERROR;
				break;
			}
		}
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (strcmp(config->stamps[i].relative_path,"etc/config/network") == 0 &&
			mesh_stamp_unchanged(root,&config->stamps[i]) != 0) {
			out->health = MESH_WAN_ERROR;
		}
	}
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return out->health == MESH_WAN_ERROR ? -1 : 0;
}
