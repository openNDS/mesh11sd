/**
 * @file portal_lldp.c
 * @brief Encode, decode, and cache portal-only LLDP announcements.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "portal_runtime.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t destination[6] = { 1, 0x80, 0xc2, 0, 0, 0x0e };
static void mac_text(const uint8_t mac[6], char text[18])
{
	snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static bool portal_id(const uint8_t id[16])
{
	static const uint8_t suffix[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
	return id[0] == 0xfd && memcmp(id + 8, suffix, 8) == 0;
}
static size_t tlv(uint8_t *out, unsigned int type, const void *value, size_t length)
{
	out[0] = (uint8_t)((type << 1) | (length >> 8));
	out[1] = (uint8_t)length;
	if (length != 0) {
		memcpy(out + 2, value, length);
	}
	return length + 2;
}
int mesh_portal_frame_encode(const struct mesh_portal_advert *advert, void *buffer, size_t capacity, size_t *length)
{
	uint8_t *out = buffer;
	uint8_t chassis[7] = {4};
	uint8_t port[7] = {3};
	uint8_t ttl[2];
	char ip[48];
	char bridge[18];
	char linklocal[48];
	char description[256];
	int written;
	size_t size = 14;
	if (advert == NULL || buffer == NULL || length == NULL || capacity < MESH_PORTAL_FRAME_MAX ||
		!portal_id(advert->id) || !mesh_mac_valid(advert->station) || !mesh_mac_valid(advert->factory) ||
		!mesh_mac_valid(advert->bridge) || advert->gateway == 0 || advert->ttl > 180 ||
		(advert->role != MESH_ROLE_MRP && advert->role != MESH_ROLE_MBP) ||
		advert->linklocal[0] != 0xfe || (advert->linklocal[1] & 0xc0) != 0x80 ||
		inet_ntop(AF_INET6, advert->linklocal, linklocal, sizeof(linklocal)) == NULL ||
		inet_ntop(AF_INET6, advert->id, ip, sizeof(ip)) == NULL) {
		return -1;
	}
	mac_text(advert->bridge, bridge);
	written = snprintf(description, sizeof(description), "mesh11sd/1 portal=%s role=%s bridge=%s gateway=%u.%u.%u.%u linklocal=%s",
					   ip, advert->role == MESH_ROLE_MRP ? "mrp" : "mbp", bridge,
					   advert->gateway >> 24, (advert->gateway >> 16) & 255, (advert->gateway >> 8) & 255, advert->gateway & 255, linklocal);
	if (written <= 0 || (size_t)written >= sizeof(description)) {
		return -1;
	}
	memcpy(out, destination, 6);
	memcpy(out + 6, advert->bridge, 6);
	out[12] = 0x88;
	out[13] = 0xcc;
	memcpy(chassis + 1, advert->factory, 6);
	memcpy(port + 1, advert->station, 6);
	ttl[0] = (uint8_t)(advert->ttl >> 8);
	ttl[1] = (uint8_t)advert->ttl;
	size += tlv(out + size, 1, chassis, sizeof(chassis));
	size += tlv(out + size, 2, port, sizeof(port));
	size += tlv(out + size, 3, ttl, sizeof(ttl));
	size += tlv(out + size, 6, description, (size_t)written);
	size += tlv(out + size, 0, NULL, 0);
	*length = size;
	return 0;
}
int mesh_portal_frame_decode(const void *buffer, size_t length, const uint8_t prefix[16], struct mesh_portal_advert *out)
{
	const uint8_t *bytes = buffer;
	struct mesh_portal_advert advert = {0};
	char description[256] = {0};
	char *token;
	char *next;
	struct in_addr address;
	size_t offset = 14;
	unsigned int seen = 0;
	unsigned int values = 0;
	bool ended = false;
	if (buffer == NULL || prefix == NULL || out == NULL || length < 14 || length > MESH_PORTAL_FRAME_MAX ||
		memcmp(bytes, destination, 6) != 0 || bytes[12] != 0x88 || bytes[13] != 0xcc || !mesh_mac_valid(bytes + 6)) {
		return -1;
	}
	memcpy(advert.bridge, bytes + 6, 6);
	while (offset + 2 <= length) {
		unsigned int type = bytes[offset] >> 1;
		size_t size = ((size_t)(bytes[offset] & 1) << 8) | bytes[offset + 1];
		unsigned int bit = type == 1 ? 1 : type == 2 ? 2 : type == 3 ? 4 : type == 6 ? 8 : 0;
		offset += 2;
		if (size > length - offset || (bit != 0 && (seen & bit) != 0)) {
			return -1;
		}
		if (type == 0) {
			ended = size == 0;
			break;
		}
		if ((seen & 7) != 7 && type != (seen == 0 ? 1U : seen == 1 ? 2U : 3U)) {
			return -1;
		}
		seen |= bit;
		if (type == 1) {
			if (size != 7 || bytes[offset] != 4 || !mesh_mac_valid(bytes + offset + 1)) {
				return -1;
			}
			memcpy(advert.factory, bytes + offset + 1, 6);
		} else if (type == 2) {
			if (size != 7 || bytes[offset] != 3 || !mesh_mac_valid(bytes + offset + 1)) {
				return -1;
			}
			memcpy(advert.station, bytes + offset + 1, 6);
		} else if (type == 3) {
			if (size != 2) {
				return -1;
			}
			advert.ttl = (unsigned int)bytes[offset] * 256 + bytes[offset + 1];
		} else if (type == 6) {
			if (size < 11 || size >= sizeof(description) || memchr(bytes + offset, '\0', size) != NULL) {
				return -1;
			}
			memcpy(description, bytes + offset, size);
		}
		offset += size;
	}
	if (!ended || seen != 15 || advert.ttl > 180 || strncmp(description, "mesh11sd/1 ", 11) != 0) {
		return -1;
	}
	next = description + 11;
	while ((token = strsep(&next, " ")) != NULL) {
		char *value = strchr(token, '=');
		unsigned int bit;
		if (value == NULL) {
			return -1;
		}
		*value++ = '\0';
		bit = strcmp(token, "portal") == 0 ? 1 : strcmp(token, "role") == 0 ? 2 :
			  strcmp(token, "bridge") == 0 ? 4 : strcmp(token, "gateway") == 0 ? 8 : strcmp(token, "linklocal") == 0 ? 16 : 0;
		if (bit == 0 || (values & bit) != 0) {
			return -1;
		}
		values |= bit;
		if (bit == 1 && inet_pton(AF_INET6, value, advert.id) != 1) {
			return -1;
		}
		if (bit == 2) {
			if (strcmp(value, "mrp") != 0 && strcmp(value, "mbp") != 0) {
				return -1;
			}
			advert.role = strcmp(value, "mrp") == 0 ? MESH_ROLE_MRP : MESH_ROLE_MBP;
		}
		if (bit == 4) {
			uint8_t bridge_mac[6];
			if (mesh_mac_parse(value, bridge_mac) != 0 || memcmp(bridge_mac, advert.bridge, 6) != 0) {
				return -1;
			}
		}
		if (bit == 16 && (inet_pton(AF_INET6, value, advert.linklocal) != 1 ||
						 advert.linklocal[0] != 0xfe || (advert.linklocal[1] & 0xc0) != 0x80)) {
			return -1;
		}
		if (bit == 8) {
			if (inet_pton(AF_INET, value, &address) != 1) {
				return -1;
			}
			advert.gateway = ntohl(address.s_addr);
		}
	}
	if (values != 31 || !portal_id(advert.id) || memcmp(advert.id, prefix, 6) != 0 ||
		!mesh_mac_valid(advert.bridge) || advert.gateway == 0 || advert.gateway >= UINT32_C(0xe0000000)) {
		return -1;
	}
	*out = advert;
	return 0;
}
int mesh_portal_hear(struct mesh_portal_runtime *runtime, const void *buffer, size_t length,
					  const uint8_t prefix[16], uint64_t now)
{
	struct mesh_portal_advert advert;
	struct mesh_portal_record *slot = NULL;
	size_t i;
	if (runtime == NULL || mesh_portal_frame_decode(buffer, length, prefix, &advert) != 0 || advert.gateway != runtime->gateway) {
		return -1;
	}
	for (i = 0; i < MESH_PORTALS_MAX; i++) {
		struct mesh_portal_record *record = &runtime->records[i];
		if (record->used && memcmp(record->advert.id, advert.id, 16) == 0) {
			if (memcmp(record->advert.factory, advert.factory, 6) != 0 || now < record->seen_seconds) {
				return -1;
			}
			slot = record;
			break;
		}
		if (!record->used || (now >= record->seen_seconds && now - record->seen_seconds > record->advert.ttl)) {
			slot = record;
		}
	}
	if (slot == NULL) {
		return -1;
	}
	if (!slot->used || memcmp(slot->advert.station, advert.station, 6) != 0 || memcmp(slot->advert.id, advert.id, 16) != 0) {
		memset(slot, 0, sizeof(*slot));
	}
	slot->advert = advert;
	slot->seen_seconds = now;
	slot->used = advert.ttl != 0;
	return 0;
}
