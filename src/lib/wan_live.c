/**
 * @file wan_live.c
 * @brief Qualify a source-bound configured DNS path on the WAN device.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "wan.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

struct route_query {
	struct nlmsghdr header;
	struct rtmsg route;
	unsigned char attributes[64];
};
static int add(struct route_query *request, unsigned int type, uint32_t value)
{
	struct rtattr *attribute;
	size_t length = RTA_ALIGN(RTA_LENGTH(sizeof(value)));
	if (request->header.nlmsg_len + length > sizeof(*request)) {
		return -1;
	}
	attribute = (struct rtattr *)((unsigned char *)request+request->header.nlmsg_len);
	attribute->rta_type = (uint16_t)type;
	attribute->rta_len = RTA_LENGTH(sizeof(value));
	memcpy(RTA_DATA(attribute),&value,sizeof(value));
	request->header.nlmsg_len += (uint32_t)length;
	return 0;
}
int mesh_wan_route_request(void *out, size_t capacity, size_t *length,
							uint32_t index, uint32_t source, uint32_t server)
{
	struct route_query request = {0};
	if (out == NULL || length == NULL || capacity < sizeof(request) || index == 0 || index > INT_MAX || source == 0 || server == 0) {
		return -1;
	}
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.route));
	request.header.nlmsg_type = RTM_GETROUTE;
	request.header.nlmsg_flags = NLM_F_REQUEST;
	request.header.nlmsg_seq = 1;
	request.route.rtm_family = AF_INET;
	request.route.rtm_dst_len = 32;
	request.route.rtm_src_len = 32;
	if (add(&request,RTA_DST,htonl(server)) != 0 || add(&request,RTA_SRC,htonl(source)) != 0 || add(&request,RTA_OIF,index) != 0) {
		return -1;
	}
	memcpy(out,&request,request.header.nlmsg_len);
	*length = request.header.nlmsg_len;
	return 0;
}
int mesh_wan_route_decode(const void *data, size_t length, uint32_t index, uint32_t source, uint32_t server)
{
	const struct rtmsg *route = data;
	const unsigned char *cursor;
	size_t remaining;
	unsigned int seen = 0;
	if (data == NULL || (uintptr_t)data % _Alignof(struct rtmsg) != 0 ||
		length < NLMSG_ALIGN(sizeof(*route)) || route->rtm_family != AF_INET ||
		route->rtm_type != RTN_UNICAST || route->rtm_dst_len != 32) {
		return -1;
	}
	cursor = (const unsigned char *)data+NLMSG_ALIGN(sizeof(*route));
	remaining = length-NLMSG_ALIGN(sizeof(*route));
	while (remaining != 0) {
		const struct rtattr *attribute = (const struct rtattr *)cursor;
		size_t aligned;
		uint32_t value;
		unsigned int bit = 0;
		if (remaining < sizeof(*attribute) || attribute->rta_len < RTA_LENGTH(0) || attribute->rta_len > remaining ||
			(aligned = RTA_ALIGN(attribute->rta_len)) > remaining) {
			return -1;
		}
		if (attribute->rta_type == RTA_MULTIPATH || attribute->rta_type == RTA_VIA) {
			return -1;
		}
		if (attribute->rta_type == RTA_OIF || attribute->rta_type == RTA_DST ||
			attribute->rta_type == RTA_SRC || attribute->rta_type == RTA_PREFSRC) {
			if (RTA_PAYLOAD(attribute) != sizeof(value)) {
				return -1;
			}
			memcpy(&value,RTA_DATA(attribute),sizeof(value));
			if (attribute->rta_type == RTA_OIF) {
				bit = 1;
				if (value != index) {
					return -1;
				}
			} else if (attribute->rta_type == RTA_DST) {
				bit = 2;
				if (ntohl(value) != server) {
					return -1;
				}
			} else {
				bit = attribute->rta_type == RTA_SRC ? 4 : 8;
				if (ntohl(value) != source) {
					return -1;
				}
			}
			if ((seen & bit) != 0) {
				return -1;
			}
			seen |= bit;
		}
		cursor += aligned;
		remaining -= aligned;
	}
	return (seen & 3) == 3 ? 0 : -1;
}
static uint16_t read16(const unsigned char *p)
{
	return (uint16_t)((unsigned int)p[0]*256+p[1]);
}
static int dns_name(const unsigned char *data, size_t size, size_t start, size_t *after, bool *root)
{
	size_t offset = start;
	unsigned int hops;
	bool jumped = false;
	size_t expanded = 0;
	for (hops = 0; hops < 128 && offset < size; hops++) {
		unsigned int count = data[offset++];
		if ((count & 0xc0) == 0xc0) {
			size_t target;
			if (offset >= size) {
				return -1;
			}
			target = ((size_t)(count & 63)<<8) | data[offset++];
			if (target >= offset-2) {
				return -1;
			}
			if (!jumped) {
				*after = offset;
			}
			jumped = true;
			offset = target;
		} else if ((count & 0xc0) != 0 || count > 63 || count > size-offset || expanded+count+1 > 255) {
			return -1;
		} else if (count == 0) {
			if (root != NULL) {
				*root = expanded == 0;
			}
			if (!jumped) {
				*after = offset;
			}
			return 0;
		} else {
			expanded += count+1;
			offset += count;
		}
	}
	return -1;
}
int mesh_wan_dns_request(void *out, size_t capacity, size_t *length, uint16_t id)
{
	unsigned char packet[17] = {0};
	if (out == NULL || length == NULL || capacity < sizeof(packet)) {
		return -1;
	}
	packet[0] = (unsigned char)(id>>8);
	packet[1] = (unsigned char)id;
	packet[2] = 1;
	packet[5] = 1;
	packet[14] = 2;
	packet[16] = 1;
	memcpy(out,packet,sizeof(packet));
	*length = sizeof(packet);
	return 0;
}
int mesh_wan_dns_response(const void *buffer, size_t length, uint16_t id)
{
	const unsigned char *data = buffer;
	size_t offset = 17;
	unsigned int count;
	unsigned int i;
	bool found = false;
	if (buffer == NULL || length < 17 || length > 4096 || read16(data) != id ||
		(data[2] & 0xfa) != 0x80 || (data[3] & 15) != 0 || read16(data+4) != 1 ||
		data[12] != 0 || read16(data+13) != 2 || read16(data+15) != 1 ||
		(count = read16(data+6)) == 0 || count > 64) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		size_t after;
		size_t end;
		uint16_t type;
		uint16_t class;
		uint16_t size;
		bool owner_root;
		bool target_root;
		if (dns_name(data,length,offset,&after,&owner_root) != 0 || length-after < 10) {
			return -1;
		}
		offset = after;
		type = read16(data+offset);
		class = read16(data+offset+2);
		size = read16(data+offset+8);
		offset += 10;
		if (size > length-offset) {
			return -1;
		}
		end = offset+size;
		if (type == 2 && class == 1) {
			if (!owner_root || size == 0 || dns_name(data,length,offset,&after,&target_root) != 0 || target_root || after != end) {
				return -1;
			}
			found = true;
		}
		offset = end;
	}
	return found ? 0 : -1;
}
static int route_matches(uint32_t index, uint32_t source, uint32_t server)
{
	struct route_query request;
	union { struct nlmsghdr align; unsigned char bytes[8192]; } reply;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	struct sockaddr_nl from;
	struct iovec vector = { .iov_base = reply.bytes, .iov_len = sizeof(reply.bytes) };
	struct msghdr message = {0};
	struct pollfd event;
	size_t length;
	ssize_t size;
	int fd;
	int result = -1;
	if (mesh_wan_route_request(&request,sizeof(request),&length,index,source,server) != 0 ||
		(fd = socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE)) < 0) {
		return -1;
	}
	if (bind(fd,(struct sockaddr *)&kernel,sizeof(kernel)) != 0 ||
		sendto(fd,&request,length,0,(struct sockaddr *)&kernel,sizeof(kernel)) != (ssize_t)length) {
		goto done;
	}
	event = (struct pollfd){ .fd = fd, .events = POLLIN };
	if (poll(&event,1,1500) <= 0) {
		goto done;
	}
	memset(&from,0,sizeof(from));
	message.msg_name = &from;
	message.msg_namelen = sizeof(from);
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	size = recvmsg(fd,&message,0);
	if (size < (ssize_t)sizeof(reply.align) || (message.msg_flags & MSG_TRUNC) != 0 || from.nl_pid != 0 ||
		reply.align.nlmsg_seq != 1 || reply.align.nlmsg_len < NLMSG_HDRLEN || reply.align.nlmsg_len > (size_t)size) {
		goto done;
	}
	if (reply.align.nlmsg_type == NLMSG_ERROR) {
		const struct nlmsgerr *error = NLMSG_DATA(&reply.align);
		if (reply.align.nlmsg_len >= NLMSG_LENGTH(sizeof(*error)) &&
			(error->error == -ENETUNREACH || error->error == -EHOSTUNREACH || error->error == -ENODEV)) {
			result = 1;
		}
	} else if (reply.align.nlmsg_type == RTM_NEWROUTE) {
		result = mesh_wan_route_decode(NLMSG_DATA(&reply.align),reply.align.nlmsg_len-NLMSG_HDRLEN,index,source,server) == 0 ? 0 : 1;
	}
done:
	close(fd);
	return result;
}
int mesh_wan_native_probe(void *context, const struct mesh_wan_evidence *evidence, uint32_t server)
{
	struct ifaddrs *addresses;
	struct ifaddrs *item;
	struct sockaddr_in source = { .sin_family = AF_INET };
	struct sockaddr_in target = { .sin_family = AF_INET, .sin_port = htons(53) };
	struct sockaddr_in actual;
	socklen_t actual_size = sizeof(actual);
	struct timeval timeout = { .tv_sec = 1 };
	unsigned char query[17];
	unsigned char answer[4096];
	char path[64];
	char link[256];
	size_t query_size;
	uint16_t id;
	uint32_t index;
	bool assigned = false;
	bool configured = false;
	size_t i;
	ssize_t size;
	int fd;
	int result;
	(void)context;
	if (evidence == NULL || memchr(evidence->device,'\0',sizeof(evidence->device)) == NULL ||
		evidence->device[0] == '\0' || evidence->prefix == 0 || evidence->prefix > 32 ||
		evidence->dns_count > MESH_WAN_DNS_MAX) {
		return -1;
	}
	for (i = 0; i < evidence->dns_count; i++) {
		configured = configured || evidence->dns[i] == server;
	}
	index = if_nametoindex(evidence->device);
	if (!configured || index == 0 || getifaddrs(&addresses) != 0) {
		return -1;
	}
	for (item = addresses; item != NULL; item = item->ifa_next) {
		if (item->ifa_addr != NULL && item->ifa_addr->sa_family == AF_INET &&
			strcmp(item->ifa_name,evidence->device) == 0 && (item->ifa_flags & IFF_UP) != 0 &&
			(item->ifa_flags & IFF_LOOPBACK) == 0 &&
			ntohl(((struct sockaddr_in *)item->ifa_addr)->sin_addr.s_addr) == evidence->source) {
			assigned = true;
		}
	}
	freeifaddrs(addresses);
	if (!assigned || snprintf(path,sizeof(path),"/sys/class/net/%s/master",evidence->device) >= (int)sizeof(path)) {
		return 1;
	}
	if (readlink(path,link,sizeof(link)) >= 0 || errno != ENOENT) {
		return 1;
	}
	result = route_matches(index,evidence->source,server);
	if (result != 0) {
		return result;
	}
	if (getrandom(&id,sizeof(id),0) != sizeof(id) || mesh_wan_dns_request(query,sizeof(query),&query_size,id) != 0 ||
		(fd = socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0)) < 0) {
		return -1;
	}
	source.sin_addr.s_addr = htonl(evidence->source);
	target.sin_addr.s_addr = htonl(server);
	result = 1;
	if (setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,evidence->device,strlen(evidence->device)+1) == 0 &&
		setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) == 0 &&
		setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) == 0 &&
		bind(fd,(struct sockaddr *)&source,sizeof(source)) == 0 &&
		connect(fd,(struct sockaddr *)&target,sizeof(target)) == 0 &&
		getsockname(fd,(struct sockaddr *)&actual,&actual_size) == 0 && actual.sin_addr.s_addr == source.sin_addr.s_addr &&
		send(fd,query,query_size,0) == (ssize_t)query_size &&
		(size = recv(fd,answer,sizeof(answer),MSG_TRUNC)) > 0 && size <= (ssize_t)sizeof(answer)) {
		result = mesh_wan_dns_response(answer,(size_t)size,id) == 0 ? 0 : 1;
	}
	close(fd);
	return result;
}
