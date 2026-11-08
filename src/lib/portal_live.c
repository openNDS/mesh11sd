/**
 * @file portal_live.c
 * @brief Probe a portal path with ICMPv6 on the selected mesh interface.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "portal_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint8_t lldp_destination[6] = {1, 0x80, 0xc2, 0, 0, 0x0e};
static bool linklocal(const uint8_t address[16])
{
	return address[0] == 0xfe && (address[1] & 0xc0) == 0x80;
}
int mesh_portal_local_advert(struct mesh_peer *peer, struct mesh_portal_advert *advert)
{
	struct ifaddrs *addresses;
	struct ifaddrs *item;
	struct mesh_portal_advert next = {0};
	bool have_station = false;
	bool have_bridge = false;
	bool have_local = false;
	char address[48];
	int result = -1;
	if (peer == NULL || peer->plan == NULL || peer->config == NULL || advert == NULL ||
		inet_pton(AF_INET6, peer->plan->identity.portal_address, next.id) != 1 || getifaddrs(&addresses) != 0) {
		return -1;
	}
	memcpy(next.factory, peer->plan->identity.factory_mac, 6);
	memcpy(next.bridge, peer->plan->identity.bridge_mac, 6);
	next.gateway = peer->config->config.portal_ipv4.address;
	next.role = peer->plan->role;
	next.ttl = MESH_PORTAL_TTL;
	for (item = addresses; item != NULL; item = item->ifa_next) {
		if (item->ifa_addr == NULL) {
			continue;
		}
		if (strcmp(item->ifa_name, peer->config->mesh_ifname) == 0 && item->ifa_addr->sa_family == AF_PACKET) {
			const struct sockaddr_ll *packet = (const struct sockaddr_ll *)item->ifa_addr;
			if (packet->sll_halen != 6 || !mesh_mac_valid(packet->sll_addr)) {
				goto done;
			}
			memcpy(next.station, packet->sll_addr, 6);
			have_station = true;
		}
		if (strcmp(item->ifa_name, peer->config->bridge_ifname) == 0 && item->ifa_addr->sa_family == AF_PACKET) {
			const struct sockaddr_ll *packet = (const struct sockaddr_ll *)item->ifa_addr;
			if (packet->sll_halen != 6 || memcmp(packet->sll_addr, next.bridge, 6) != 0) {
				goto done;
			}
			have_bridge = true;
		}
		if (strcmp(item->ifa_name, peer->config->bridge_ifname) == 0 && item->ifa_addr->sa_family == AF_INET6) {
			const struct sockaddr_in6 *socket_address = (const struct sockaddr_in6 *)item->ifa_addr;
			if (IN6_IS_ADDR_LINKLOCAL(&socket_address->sin6_addr)) {
				if (have_local && memcmp(next.linklocal, &socket_address->sin6_addr, 16) != 0) {
					goto done;
				}
				memcpy(next.linklocal, &socket_address->sin6_addr, 16);
				have_local = true;
			}
		}
	}
	if (have_station && have_bridge && have_local && inet_ntop(AF_INET6, next.linklocal, address, sizeof(address)) != NULL &&
		mesh_probe_ipv6_ready(peer->config->bridge_ifname, address) == 0) {
		*advert = next;
		result = 0;
	}
done:
	freeifaddrs(addresses);
	return result;
}
static int packet_socket(uint32_t index, uint16_t protocol)
{
	struct sockaddr_ll address = { .sll_family = AF_PACKET, .sll_protocol = 0, .sll_ifindex = (int)index };
	int fd;
	if (index == 0 || index > INT32_MAX) {
		return -1;
	}
	address.sll_protocol = htons(protocol);
	fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(protocol));
	if (fd < 0) {
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}
int mesh_portal_lldp_open(uint32_t index)
{
	struct packet_mreq multicast = { .mr_ifindex = (int)index, .mr_type = PACKET_MR_MULTICAST, .mr_alen = 6 };
	int fd = packet_socket(index, 0x88cc);
	if (fd < 0) {
		return -1;
	}
	memcpy(multicast.mr_address, lldp_destination, 6);
	if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &multicast, sizeof(multicast)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}
int mesh_portal_lldp_send(int fd, uint32_t index, const struct mesh_portal_advert *advert)
{
	uint8_t packet[MESH_PORTAL_FRAME_MAX];
	size_t length;
	struct sockaddr_ll address = { .sll_family = AF_PACKET, .sll_protocol = htons(0x88cc),
								   .sll_ifindex = (int)index, .sll_halen = 6 };
	if (fd < 0 || mesh_portal_frame_encode(advert, packet, sizeof(packet), &length) != 0) {
		return -1;
	}
	memcpy(address.sll_addr, lldp_destination, 6);
	return sendto(fd, packet, length, MSG_DONTWAIT, (struct sockaddr *)&address, sizeof(address)) == (ssize_t)length ? 0 : -1;
}
int mesh_portal_lldp_receive(int fd, uint32_t index, struct mesh_portal_runtime *runtime,
							const uint8_t prefix[16], uint64_t now)
{
	uint8_t packet[MESH_PORTAL_FRAME_MAX];
	struct sockaddr_ll source;
	struct iovec vector;
	struct msghdr message;
	ssize_t size;
	unsigned int round;
	for (round = 0; round < 64; round++) {
		memset(&source, 0, sizeof(source));
		memset(&message, 0, sizeof(message));
		vector = (struct iovec){ .iov_base = packet, .iov_len = sizeof(packet) };
		message.msg_name = &source;
		message.msg_namelen = sizeof(source);
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		size = recvmsg(fd, &message, MSG_DONTWAIT);
		if (size < 0) {
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
		}
		if (size > 0 && (message.msg_flags & MSG_TRUNC) == 0 && (uint32_t)source.sll_ifindex == index &&
			source.sll_pkttype != PACKET_OUTGOING) {
			/* Invalid/untrusted announcements are dropped, never interpreted
			 * as shell input or an instruction to change another interface. */
			mesh_portal_hear(runtime, packet, (size_t)size, prefix, now);
		}
	}
	return 0;
}
static uint16_t checksum(const uint8_t *packet)
{
	uint32_t sum = 24 + 58;
	unsigned int i;
	/* IPv6 pseudo-header source/destination plus fixed ICMPv6 payload. */
	for (i = 22; i < 54; i += 2) {
		sum += (unsigned int)packet[i] * 256 + packet[i + 1];
	}
	for (i = 54; i < 78; i += 2) {
		sum += (unsigned int)packet[i] * 256 + packet[i + 1];
	}
	while (sum >> 16) {
		sum = (sum & 65535) + (sum >> 16);
	}
	return (uint16_t)~sum;
}
int mesh_portal_echo_build(const struct mesh_portal_advert *local, const struct mesh_portal_advert *remote,
						   const uint8_t nonce[16], void *buffer, size_t capacity, size_t *length)
{
	uint8_t *packet = buffer;
	uint16_t value;
	if (local == NULL || remote == NULL || nonce == NULL || buffer == NULL || length == NULL || capacity < 78 ||
		!mesh_mac_valid(local->bridge) || !mesh_mac_valid(remote->bridge) ||
		!linklocal(local->linklocal) || !linklocal(remote->linklocal)) {
		return -1;
	}
	memset(packet, 0, 78);
	memcpy(packet, remote->bridge, 6);
	memcpy(packet + 6, local->bridge, 6);
	packet[12] = 0x86;
	packet[13] = 0xdd;
	packet[14] = 0x60;
	packet[19] = 24;
	packet[20] = 58;
	packet[21] = 255;
	memcpy(packet + 22, local->linklocal, 16);
	memcpy(packet + 38, remote->linklocal, 16);
	packet[54] = 128;
	memcpy(packet + 58, nonce, 4);
	memcpy(packet + 62, nonce, 16);
	value = checksum(packet);
	packet[56] = (uint8_t)(value >> 8);
	packet[57] = (uint8_t)value;
	*length = 78;
	return 0;
}
int mesh_portal_echo_reply(const struct mesh_portal_advert *local, const struct mesh_portal_advert *remote,
						   const uint8_t nonce[16], const void *buffer, size_t length)
{
	const uint8_t *packet = buffer;
	if (local == NULL || remote == NULL || nonce == NULL || buffer == NULL || length < 78 ||
		memcmp(packet, local->bridge, 6) != 0 || memcmp(packet + 6, remote->bridge, 6) != 0 ||
		packet[12] != 0x86 || packet[13] != 0xdd || packet[14] >> 4 != 6 ||
		packet[18] != 0 || packet[19] != 24 || packet[20] != 58 || packet[21] == 0 ||
		memcmp(packet + 22, remote->linklocal, 16) != 0 || memcmp(packet + 38, local->linklocal, 16) != 0 ||
		packet[54] != 129 || packet[55] != 0 || memcmp(packet + 58, nonce, 4) != 0 ||
		memcmp(packet + 62, nonce, 16) != 0 || checksum(packet) != 0) {
		return -1;
	}
	return 0;
}
int mesh_portal_live_probe(void *context, const struct mesh_portal_advert *remote)
{
	struct mesh_peer *peer = context;
	struct mesh_portal_advert local;
	uint8_t nonce[16];
	uint8_t packet[MESH_PORTAL_FRAME_MAX];
	struct sockaddr_ll address;
	socklen_t address_size;
	struct pollfd event;
	size_t length;
	ssize_t size;
	uint64_t started;
	unsigned int rounds;
	uint32_t index;
	int fd;
	int result = -1;
	if (mesh_portal_local_advert(peer, &local) != 0 || getrandom(nonce, sizeof(nonce), GRND_NONBLOCK) != sizeof(nonce) ||
		mesh_portal_echo_build(&local, remote, nonce, packet, sizeof(packet), &length) != 0 ||
		(index = if_nametoindex(peer->config->mesh_ifname)) == 0) {
		return -1;
	}
	fd = packet_socket(index, 0x86dd);
	if (fd < 0) {
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sll_family = AF_PACKET;
	address.sll_protocol = htons(0x86dd);
	address.sll_ifindex = (int)index;
	address.sll_halen = 6;
	memcpy(address.sll_addr, remote->bridge, 6);
	if (sendto(fd, packet, length, 0, (struct sockaddr *)&address, sizeof(address)) != (ssize_t)length) {
		goto done;
	}
	started = peer->transport.now_ms(peer->transport.context);
	for (rounds = 0; rounds < 64 && peer->transport.now_ms(peer->transport.context) - started < 1500; rounds++) {
		event = (struct pollfd){ .fd = fd, .events = POLLIN };
		if (poll(&event, 1, 100) <= 0) {
			continue;
		}
		address_size = sizeof(address);
		size = recvfrom(fd, packet, sizeof(packet), MSG_TRUNC, (struct sockaddr *)&address, &address_size);
		if (size >= 0 && (size_t)size <= sizeof(packet) && (uint32_t)address.sll_ifindex == index &&
			address.sll_pkttype != PACKET_OUTGOING && mesh_portal_echo_reply(&local, remote, nonce, packet, (size_t)size) == 0) {
			result = 0;
			break;
		}
	}
done:
	close(fd);
	return result;
}
int mesh_portal_live_measure(void *context, uint32_t index, const uint8_t station[6],
							struct mesh_path_sample *path, struct mesh_station_sample *sample)
{
	struct mesh_peer *peer = context;
	if (peer == NULL || peer->config == NULL || if_nametoindex(peer->config->mesh_ifname) != index) {
		return -1;
	}
	return mesh_probe_path_station(index, station, path, sample);
}
int mesh_portal_live_client_ready(void *context)
{
	struct mesh_peer *peer = context;
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	struct blob_attr *proto;
	struct blob_attr *device;
	struct blob_attr *armed;
	struct blob_attr *up;
	struct blob_attr *pending;
	int result = -1;
	if (mesh_message_init(&request) != 0) {
		return -1;
	}
	if (peer == NULL || peer->plan == NULL || peer->config == NULL || peer->plan->role != MESH_ROLE_PEER ||
		mesh_peer_call(peer, "network.interface.mesh11sd_v8_client", "status", &request, &reply) != 0) {
		goto done;
	}
	proto = mesh_root_field(&reply, "proto", BLOBMSG_TYPE_STRING);
	device = mesh_root_field(&reply, "device", BLOBMSG_TYPE_STRING);
	armed = mesh_root_field(&reply, "autostart", BLOBMSG_TYPE_BOOL);
	up = mesh_root_field(&reply, "up", BLOBMSG_TYPE_BOOL);
	pending = mesh_root_field(&reply, "pending", BLOBMSG_TYPE_BOOL);
	if (proto != NULL && device != NULL && armed != NULL && strcmp(blobmsg_get_string(proto), "dhcp") == 0 &&
		strcmp(blobmsg_get_string(device), peer->config->bridge_ifname) == 0 && blobmsg_get_bool(armed)) {
		result = ((up != NULL && blobmsg_get_bool(up)) || (pending != NULL && blobmsg_get_bool(pending))) ? 0 : 1;
	}
done:
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}
int mesh_portal_live_renew(void *context)
{
	struct mesh_peer *peer = context;
	struct mesh_message request;
	struct mesh_runtime_data reply = {0};
	int result;
	if (mesh_portal_live_client_ready(context) != 0) {
		return -1;
	}
	if (mesh_message_init(&request) != 0) {
		return -1;
	}
	result = mesh_peer_call(peer, "network.interface.mesh11sd_v8_client", "renew", &request, &reply);
	mesh_message_clear(&request);
	mesh_runtime_data_clear(&reply);
	return result;
}
