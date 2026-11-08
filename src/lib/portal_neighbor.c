/**
 * @file portal_neighbor.c
 * @brief Pin and restore this node's IPv4 gateway neighbor entry.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "portal_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct neighbor_request {
	struct nlmsghdr header;
	struct ndmsg neighbor;
	unsigned char attributes[64];
};
static int add(struct neighbor_request *request, uint16_t type, const void *data, size_t length)
{
	struct rtattr *attribute;
	size_t size = RTA_ALIGN(RTA_LENGTH(length));
	if (request->header.nlmsg_len + size > sizeof(*request)) {
		return -1;
	}
	attribute = (struct rtattr *)((unsigned char *)request + request->header.nlmsg_len);
	attribute->rta_type = type;
	attribute->rta_len = (uint16_t)RTA_LENGTH(length);
	memcpy(RTA_DATA(attribute), data, length);
	request->header.nlmsg_len += (uint32_t)size;
	return 0;
}
int mesh_portal_neighbor_request(void *buffer, size_t capacity, size_t *length,
								 uint32_t ifindex, uint32_t gateway, const struct mesh_neighbor *neighbor)
{
	struct neighbor_request request = {0};
	uint32_t address = htonl(gateway);
	if (buffer == NULL || length == NULL || ifindex == 0 || ifindex > INT_MAX || gateway == 0 ||
		capacity < sizeof(request) || (neighbor != NULL && neighbor->exists &&
		neighbor->type != RTN_UNICAST)) {
		return -1;
	}
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.neighbor));
	request.header.nlmsg_type = neighbor == NULL ? RTM_GETNEIGH : neighbor->exists ? RTM_NEWNEIGH : RTM_DELNEIGH;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	if (neighbor != NULL && neighbor->exists) {
		request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
	}
	request.header.nlmsg_seq = 1;
	request.neighbor.ndm_family = AF_INET;
	request.neighbor.ndm_ifindex = (int)ifindex;
	if (neighbor != NULL && neighbor->exists) {
		request.neighbor.ndm_state = neighbor->state;
		request.neighbor.ndm_flags = neighbor->flags;
		request.neighbor.ndm_type = neighbor->type;
	}
	if (add(&request, NDA_DST, &address, sizeof(address)) != 0 ||
		(neighbor != NULL && neighbor->exists && mesh_mac_valid(neighbor->mac) &&
		 add(&request, NDA_LLADDR, neighbor->mac, 6) != 0)) {
		return -1;
	}
	memcpy(buffer, &request, request.header.nlmsg_len);
	*length = request.header.nlmsg_len;
	return 0;
}
int mesh_portal_neighbor_decode(const void *data, size_t length, uint32_t ifindex,
								uint32_t gateway, struct mesh_neighbor *out)
{
	const struct ndmsg *neighbor = data;
	const unsigned char *cursor;
	size_t remaining;
	bool have_address = false;
	bool have_mac = false;
	struct mesh_neighbor result = { .exists = true };
	if (data == NULL || out == NULL || length < NLMSG_ALIGN(sizeof(*neighbor)) ||
		(uintptr_t)data % _Alignof(struct ndmsg) != 0 || neighbor->ndm_family != AF_INET ||
		neighbor->ndm_ifindex <= 0 || (uint32_t)neighbor->ndm_ifindex != ifindex) {
		return -1;
	}
	cursor = (const unsigned char *)data + NLMSG_ALIGN(sizeof(*neighbor));
	remaining = length - NLMSG_ALIGN(sizeof(*neighbor));
	while (remaining != 0) {
		const struct rtattr *attribute = (const struct rtattr *)cursor;
		size_t size;
		if (remaining < sizeof(*attribute) || attribute->rta_len < RTA_LENGTH(0) ||
			attribute->rta_len > remaining || (size = RTA_ALIGN(attribute->rta_len)) > remaining) {
			return -1;
		}
		if (attribute->rta_type == NDA_DST) {
			uint32_t address;
			if (have_address || RTA_PAYLOAD(attribute) != sizeof(address)) {
				return -1;
			}
			memcpy(&address, RTA_DATA(attribute), sizeof(address));
			if (ntohl(address) != gateway) {
				return -1;
			}
			have_address = true;
		} else if (attribute->rta_type == NDA_LLADDR) {
			if (have_mac || RTA_PAYLOAD(attribute) != 6) {
				return -1;
			}
			memcpy(result.mac, RTA_DATA(attribute), 6);
			if (!mesh_mac_valid(result.mac)) {
				return -1;
			}
			have_mac = true;
		}
		remaining -= size;
		cursor += size;
	}
	result.state = neighbor->ndm_state;
	result.flags = neighbor->ndm_flags;
	result.type = neighbor->ndm_type;
	if (!have_address || (!have_mac && (result.state & (NUD_PERMANENT | NUD_NOARP | NUD_REACHABLE | NUD_PROBE | NUD_STALE | NUD_DELAY)) != 0)) {
		return -1;
	}
	*out = result;
	return 0;
}
static int exchange(uint32_t ifindex, uint32_t gateway, const struct mesh_neighbor *desired, struct mesh_neighbor *out)
{
	union {
		struct nlmsghdr align;
		unsigned char bytes[4096];
	} input;
	struct neighbor_request request;
	struct sockaddr_nl address = { .nl_family = AF_NETLINK };
	struct sockaddr_nl source;
	struct iovec vector;
	struct msghdr message;
	struct pollfd event;
	struct mesh_neighbor found = {0};
	size_t length;
	size_t remaining;
	unsigned int round;
	int fd;
	int result = -1;
	bool have_reply = false;
	ssize_t received;
	if ((desired == NULL && out == NULL) || mesh_portal_neighbor_request(&request, sizeof(request), &length, ifindex, gateway, desired) != 0) {
		return -1;
	}
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
	if (fd < 0) {
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
		sendto(fd, &request, length, 0, (struct sockaddr *)&address, sizeof(address)) != (ssize_t)length) {
		goto done;
	}
	for (round = 0; round < 4; round++) {
		event = (struct pollfd){ .fd = fd, .events = POLLIN };
		if (poll(&event, 1, 500) <= 0) {
			goto done;
		}
		memset(&source, 0, sizeof(source));
		vector = (struct iovec){ .iov_base = input.bytes, .iov_len = sizeof(input.bytes) };
		memset(&message, 0, sizeof(message));
		message.msg_name = &source;
		message.msg_namelen = sizeof(source);
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		received = recvmsg(fd, &message, 0);
		if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0 || source.nl_pid != 0) {
			goto done;
		}
		remaining = (size_t)received;
		length = 0;
		while (remaining != 0) {
			const struct nlmsghdr *header = (const struct nlmsghdr *)(input.bytes + length);
			size_t size;
			if (remaining < sizeof(*header) || header->nlmsg_len < NLMSG_HDRLEN || header->nlmsg_len > remaining ||
				(size = NLMSG_ALIGN(header->nlmsg_len)) > remaining || header->nlmsg_seq != 1) {
				goto done;
			}
			if (header->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *error = NLMSG_DATA(header);
				if (header->nlmsg_len < NLMSG_LENGTH(sizeof(*error))) {
					goto done;
				}
				if (desired == NULL && error->error == -ENODEV) {
					*out = (struct mesh_neighbor){0};
					result = 1;
				} else if (desired == NULL && (error->error == -ENOENT || error->error == -ESRCH)) {
					*out = (struct mesh_neighbor){0};
					result = 0;
				} else if (error->error == 0 && (desired != NULL || have_reply)) {
					if (out != NULL) {
						*out = found;
					}
					result = 0;
				}
				goto done;
			}
			if (desired != NULL || have_reply || header->nlmsg_type != RTM_NEWNEIGH ||
				mesh_portal_neighbor_decode(NLMSG_DATA(header), header->nlmsg_len - NLMSG_HDRLEN,
										   ifindex, gateway, &found) != 0) {
				goto done;
			}
			have_reply = true;
			remaining -= size;
			length += size;
		}
	}
done:
	close(fd);
	return result;
}
int mesh_portal_native_get(void *context, uint32_t ifindex, uint32_t gateway, struct mesh_neighbor *out)
{
	(void)context;
	return out == NULL ? -1 : exchange(ifindex, gateway, NULL, out);
}
int mesh_portal_native_set(void *context, uint32_t ifindex, uint32_t gateway, const struct mesh_neighbor *neighbor)
{
	(void)context;
	return neighbor == NULL ? -1 : exchange(ifindex, gateway, neighbor, NULL);
}
