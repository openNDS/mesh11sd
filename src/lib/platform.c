/**
 * @file platform.c
 * @brief Query bridge identity, nl80211 radios, stations, and routes.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "platform.h"

#include <errno.h>
#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct attributes {
	const unsigned char *cursor;
	size_t remaining;
	bool malformed;
};

static const struct nlattr *attribute_next(struct attributes *attributes)
{
	const struct nlattr *attribute;
	size_t aligned;

	if (attributes->remaining == 0 || attributes->malformed) {
		return NULL;
	}
	if (attributes->remaining < sizeof(*attribute)) {
		attributes->malformed = true;
		return NULL;
	}
	attribute = (const struct nlattr *)attributes->cursor;
	if (attribute->nla_len < NLA_HDRLEN || attribute->nla_len > attributes->remaining) {
		attributes->malformed = true;
		return NULL;
	}
	aligned = NLA_ALIGN(attribute->nla_len);
	if (aligned > attributes->remaining) {
		attributes->malformed = true;
		return NULL;
	}
	attributes->cursor += aligned;
	attributes->remaining -= aligned;
	return attribute;
}

static const void *attribute_data(const struct nlattr *attribute)
{
	return (const unsigned char *)attribute + NLA_HDRLEN;
}

static size_t attribute_length(const struct nlattr *attribute)
{
	return attribute->nla_len - NLA_HDRLEN;
}

static struct attributes attribute_children(const struct nlattr *attribute)
{
	struct attributes children;

	children.cursor = attribute_data(attribute);
	children.remaining = attribute_length(attribute);
	children.malformed = false;
	return children;
}

static unsigned int attribute_type(const struct nlattr *attribute)
{
	return attribute->nla_type & NLA_TYPE_MASK;
}

static bool attribute_u32(const struct nlattr *attribute, uint32_t *value)
{
	if (attribute_length(attribute) != sizeof(*value)) {
		return false;
	}
	memcpy(value, attribute_data(attribute), sizeof(*value));
	return true;
}

int mesh_decode_bridge(const void *data, size_t length, uint8_t mac[6])
{
	struct attributes attrs;
	struct attributes info;
	const struct nlattr *attr;
	const struct nlattr *item;
	uint8_t candidate[6];
	bool have_mac;
	bool bridge;

	if (data == NULL || mac == NULL || length < sizeof(struct ifinfomsg) ||
		(uintptr_t)data % _Alignof(struct nlattr) != 0) {
		return -1;
	}
	attrs.cursor = (const unsigned char *)data + NLMSG_ALIGN(sizeof(struct ifinfomsg));
	attrs.remaining = length - NLMSG_ALIGN(sizeof(struct ifinfomsg));
	attrs.malformed = false;
	have_mac = false;
	bridge = false;
	while ((attr = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attr) == IFLA_ADDRESS) {
			if (attribute_length(attr) != 6) {
				return -1;
			}
			memcpy(candidate, attribute_data(attr), 6);
			have_mac = mesh_mac_valid(candidate);
		}
		if (attribute_type(attr) == IFLA_LINKINFO) {
			info = attribute_children(attr);
			while ((item = attribute_next(&info)) != NULL) {
				if (attribute_type(item) == IFLA_INFO_KIND && attribute_length(item) == 7 &&
					memcmp(attribute_data(item), "bridge", 7) == 0) {
					bridge = true;
				}
			}
			if (info.malformed) {
				return -1;
			}
		}
	}
	if (attrs.malformed || !bridge || !have_mac) {
		return -1;
	}
	memcpy(mac, candidate, 6);
	return 0;
}

static int decode_frequencies(const struct nlattr *list, uint32_t frequency,
							  struct mesh_radio *radio)
{
	struct attributes entries;
	struct attributes attrs;
	const struct nlattr *entry;
	const struct nlattr *attr;
	uint32_t freq;
	bool disabled;
	bool no_ir;
	bool radar;

	entries = attribute_children(list);
	while ((entry = attribute_next(&entries)) != NULL) {
		attrs = attribute_children(entry);
		freq = 0;
		disabled = false;
		no_ir = false;
		radar = false;
		while ((attr = attribute_next(&attrs)) != NULL) {
			switch (attribute_type(attr)) {
			case NL80211_FREQUENCY_ATTR_FREQ:
				if (!attribute_u32(attr, &freq)) {
					return -1;
				}
				break;
			case NL80211_FREQUENCY_ATTR_DISABLED:
				disabled = true;
				break;
			case NL80211_FREQUENCY_ATTR_NO_IR:
				no_ir = true;
				break;
			case NL80211_FREQUENCY_ATTR_RADAR:
				radar = true;
				break;
			}
		}
		if (attrs.malformed) {
			return -1;
		}
		if (freq == frequency) {
			radio->frequency_supported = true;
			radio->frequency_disabled |= disabled;
			radio->no_initiate_radiation |= no_ir;
			radio->radar_required |= radar;
		}
	}
	return entries.malformed ? -1 : 0;
}

static int decode_combinations(const struct nlattr *list, struct mesh_radio *radio)
{
	struct attributes entries = attribute_children(list);
	struct attributes attrs;
	struct attributes limits;
	struct attributes fields;
	struct attributes types;
	const struct nlattr *entry;
	const struct nlattr *attr;
	const struct nlattr *limit;
	const struct nlattr *field;
	const struct nlattr *type;
	struct mesh_combination combination;
	struct mesh_iflimit item;
	uint32_t number;

	while ((entry = attribute_next(&entries)) != NULL) {
		memset(&combination, 0, sizeof(combination));
		attrs = attribute_children(entry);
		while ((attr = attribute_next(&attrs)) != NULL) {
			switch (attribute_type(attr)) {
			case NL80211_IFACE_COMB_MAXNUM:
				if (!attribute_u32(attr, &combination.maximum)) {
					return -1;
				}
				break;
			case NL80211_IFACE_COMB_NUM_CHANNELS:
				if (!attribute_u32(attr, &combination.channels)) {
					return -1;
				}
				break;
			case NL80211_IFACE_COMB_STA_AP_BI_MATCH:
				if (attribute_length(attr) != 0) {
					return -1;
				}
				combination.infrastructure_beacon_match = true;
				break;
			case NL80211_IFACE_COMB_BI_MIN_GCD:
				if (!attribute_u32(attr, &combination.beacon_min_gcd)) {
					return -1;
				}
				break;
			case NL80211_IFACE_COMB_LIMITS:
				limits = attribute_children(attr);
				while ((limit = attribute_next(&limits)) != NULL) {
					memset(&item, 0, sizeof(item));
					fields = attribute_children(limit);
					while ((field = attribute_next(&fields)) != NULL) {
						if (attribute_type(field) == NL80211_IFACE_LIMIT_MAX) {
							if (!attribute_u32(field, &item.maximum)) {
								return -1;
							}
						} else if (attribute_type(field) == NL80211_IFACE_LIMIT_TYPES) {
							types = attribute_children(field);
							while ((type = attribute_next(&types)) != NULL) {
								number = attribute_type(type);
								if (number >= 64 || attribute_length(type) != 0) {
									return -1;
								}
								item.types |= UINT64_C(1) << number;
							}
							if (types.malformed) {
								return -1;
							}
						}
					}
					if (fields.malformed || item.maximum == 0 || item.types == 0 ||
						combination.limit_count == MESH_LIMITS_MAX) {
						return -1;
					}
					combination.limits[combination.limit_count++] = item;
				}
				if (limits.malformed) {
					return -1;
				}
				break;
			}
		}
		if (attrs.malformed || combination.maximum == 0 || combination.channels == 0 ||
			combination.limit_count == 0 || radio->combination_count == MESH_COMBINATIONS_MAX) {
			return -1;
		}
		radio->combinations[radio->combination_count++] = combination;
	}
	return entries.malformed ? -1 : 0;
}

int mesh_decode_wiphy(const void *data, size_t length, uint32_t frequency,
					 struct mesh_radio *radio)
{
	struct attributes attrs;
	struct attributes children;
	struct attributes bandattrs;
	const struct nlattr *attr;
	const struct nlattr *item;
	const struct nlattr *bandattr;
	const char *name;
	size_t name_length;
	struct mesh_radio next;

	if (data == NULL || radio == NULL || length < GENL_HDRLEN || frequency == 0 ||
		(uintptr_t)data % _Alignof(struct nlattr) != 0) {
		return -1;
	}
	next = *radio;
	attrs.cursor = (const unsigned char *)data + GENL_HDRLEN;
	attrs.remaining = length - GENL_HDRLEN;
	attrs.malformed = false;
	while ((attr = attribute_next(&attrs)) != NULL) {
		switch (attribute_type(attr)) {
		case NL80211_ATTR_WIPHY_NAME:
			name = attribute_data(attr);
			name_length = attribute_length(attr);
			if (name_length < 2 || name_length > sizeof(next.phy) ||
				name[name_length - 1] != '\0' || strlen(name) + 1 != name_length) {
				return -1;
			}
			memset(next.phy, 0, sizeof(next.phy));
			memcpy(next.phy, name, name_length);
			break;
		case NL80211_ATTR_SUPPORTED_IFTYPES:
			children = attribute_children(attr);
			while ((item = attribute_next(&children)) != NULL) {
				if (attribute_type(item) == NL80211_IFTYPE_MESH_POINT) {
					next.mesh_supported = true;
				}
			}
			if (children.malformed) {
				return -1;
			}
			break;
		case NL80211_ATTR_WIPHY_RADIOS:
			children = attribute_children(attr);
			while ((item = attribute_next(&children)) != NULL) {
				next.multiple_hw_radios = true;
			}
			if (children.malformed) {
				return -1;
			}
			break;
		case NL80211_ATTR_INTERFACE_COMBINATIONS:
			if (decode_combinations(attr, &next) != 0) {
				return -1;
			}
			break;
		case NL80211_ATTR_WIPHY_BANDS:
			children = attribute_children(attr);
			while ((item = attribute_next(&children)) != NULL) {
				bandattrs = attribute_children(item);
				while ((bandattr = attribute_next(&bandattrs)) != NULL) {
					if (attribute_type(bandattr) == NL80211_BAND_ATTR_FREQS &&
						decode_frequencies(bandattr, frequency, &next) < 0) {
						return -1;
					}
				}
				if (bandattrs.malformed) {
					return -1;
				}
			}
			if (children.malformed) {
				return -1;
			}
			break;
		}
	}
	if (attrs.malformed) {
		return -1;
	}
	*radio = next;
	return 0;
}

static int netlink_socket(int protocol)
{
	struct sockaddr_nl local;
	struct timeval timeout;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
	if (fd < 0) {
		return -1;
	}
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int request_send(int fd, const void *data, size_t length)
{
	struct sockaddr_nl kernel;
	ssize_t sent;

	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	do {
		sent = sendto(fd, data, length, 0, (struct sockaddr *)&kernel, sizeof(kernel));
	} while (sent < 0 && errno == EINTR);
	return sent == (ssize_t)length ? 0 : -1;
}

typedef int (*decode_callback)(const void *, size_t, void *);

static int replies(int fd, uint32_t sequence, uint16_t expected_type,
				   decode_callback decode, void *context, bool allow_empty)
{
	/* Netlink buffers and nested attributes are naturally aligned. */
	union {
		struct nlmsghdr alignment;
		unsigned char data[65536];
	} buffer;
	struct sockaddr_nl sender;
	struct iovec iov;
	struct msghdr message;
	const struct nlmsghdr *header;
	const struct nlmsgerr *error;
	size_t remaining;
	size_t step;
	ssize_t received;
	bool decoded;
	unsigned int rounds;

	decoded = false;
	for (rounds = 0; rounds < 128; rounds++) {
		memset(&message, 0, sizeof(message));
		memset(&sender, 0, sizeof(sender));
		iov.iov_base = buffer.data;
		iov.iov_len = sizeof(buffer.data);
		message.msg_name = &sender;
		message.msg_namelen = sizeof(sender);
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		do {
			received = recvmsg(fd, &message, 0);
		} while (received < 0 && errno == EINTR);
		if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0 || sender.nl_pid != 0) {
			return -1;
		}
		remaining = (size_t)received;
		header = (const struct nlmsghdr *)buffer.data;
		while (remaining != 0) {
			if (remaining < sizeof(*header) || header->nlmsg_len < NLMSG_HDRLEN ||
				header->nlmsg_len > remaining) {
				return -1;
			}
			step = NLMSG_ALIGN(header->nlmsg_len);
			if (step > remaining || header->nlmsg_seq != sequence ||
				(header->nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
				return -1;
			}
			if (header->nlmsg_type == NLMSG_ERROR) {
				if (header->nlmsg_len < NLMSG_LENGTH(sizeof(*error))) {
					return -1;
				}
				error = NLMSG_DATA(header);
				return error->error == 0 && decoded ? 0 : -1;
			}
			if (header->nlmsg_type == NLMSG_DONE) {
				/* DONE may carry a negative dump status. */
				if (header->nlmsg_len >= NLMSG_LENGTH(sizeof(int))) {
					int status;

					memcpy(&status, NLMSG_DATA(header), sizeof(status));
					if (status != 0) {
						return -1;
					}
				}
				return decoded || allow_empty ? 0 : -1;
			}
			if (header->nlmsg_type != expected_type ||
				decode(NLMSG_DATA(header), header->nlmsg_len - NLMSG_HDRLEN, context) < 0) {
				return -1;
			}
			decoded = true;
			remaining -= step;
			header = (const struct nlmsghdr *)((const unsigned char *)header + step);
		}
	}
	return -1;
}

static int decode_bridge_cb(const void *data, size_t length, void *context)
{
	return mesh_decode_bridge(data, length, context);
}

enum mesh_error mesh_probe_bridge(const char *ifname, uint8_t mac[6])
{
	struct {
		struct nlmsghdr header;
		struct ifinfomsg link;
	} request;
	uint8_t result[6];
	unsigned int index;
	int fd;
	int status;

	if (ifname == NULL || mac == NULL || strlen(ifname) >= IF_NAMESIZE) {
		return MESH_E_INPUT;
	}
	index = if_nametoindex(ifname);
	if (index == 0) {
		return MESH_E_PLATFORM;
	}
	fd = netlink_socket(NETLINK_ROUTE);
	if (fd < 0) {
		return MESH_E_PLATFORM;
	}
	memset(&request, 0, sizeof(request));
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.link));
	request.header.nlmsg_type = RTM_GETLINK;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	request.header.nlmsg_seq = 1;
	request.link.ifi_family = AF_UNSPEC;
	request.link.ifi_index = (int)index;
	status = request_send(fd, &request, request.header.nlmsg_len);
	if (status == 0) {
		status = replies(fd, 1, RTM_NEWLINK, decode_bridge_cb, result, false);
	}
	close(fd);
	if (status < 0) {
		return MESH_E_PLATFORM;
	}
	memcpy(mac, result, 6);
	return MESH_OK;
}

struct generic_request {
	struct nlmsghdr header;
	struct genlmsghdr generic;
	unsigned char attributes[64];
};

static void generic_init(struct generic_request *request, uint16_t family,
						 uint8_t command, uint32_t sequence)
{
	memset(request, 0, sizeof(*request));
	request->header.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	request->header.nlmsg_type = family;
	request->header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	request->header.nlmsg_seq = sequence;
	request->generic.cmd = command;
	request->generic.version = 1;
}

static int generic_attribute(struct generic_request *request, uint16_t type,
							 const void *value, size_t length)
{
	struct nlattr *attribute;
	size_t total;

	total = NLA_ALIGN(NLA_HDRLEN + length);
	if (request->header.nlmsg_len + total > sizeof(*request)) {
		return -1;
	}
	attribute = (struct nlattr *)((unsigned char *)request + request->header.nlmsg_len);
	attribute->nla_type = type;
	attribute->nla_len = (uint16_t)(NLA_HDRLEN + length);
	if (length != 0) {
		memcpy((unsigned char *)attribute + NLA_HDRLEN, value, length);
	}
	request->header.nlmsg_len += (uint32_t)total;
	return 0;
}

static int decode_family(const void *data, size_t length, void *context)
{
	struct attributes attrs;
	const struct nlattr *attribute;
	uint16_t *family;

	if (length < GENL_HDRLEN) {
		return -1;
	}
	family = context;
	attrs.cursor = (const unsigned char *)data + GENL_HDRLEN;
	attrs.remaining = length - GENL_HDRLEN;
	attrs.malformed = false;
	while ((attribute = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attribute) == CTRL_ATTR_FAMILY_ID) {
			if (attribute_length(attribute) != sizeof(*family)) {
				return -1;
			}
			memcpy(family, attribute_data(attribute), sizeof(*family));
		}
	}
	return attrs.malformed || *family < NLMSG_MIN_TYPE ? -1 : 0;
}

struct wiphy_context {
	uint32_t frequency;
	struct mesh_radio radio;
};

static int decode_wiphy_cb(const void *data, size_t length, void *context)
{
	struct wiphy_context *wiphy;

	wiphy = context;
	return mesh_decode_wiphy(data, length, wiphy->frequency, &wiphy->radio);
}

enum mesh_error mesh_probe_radio(uint32_t wiphy, uint32_t frequency,
								struct mesh_radio *radio)
{
	struct generic_request request;
	struct wiphy_context context;
	uint16_t family;
	int fd;
	int status;

	if (radio == NULL || frequency == 0) {
		return MESH_E_INPUT;
	}
	fd = netlink_socket(NETLINK_GENERIC);
	if (fd < 0) {
		return MESH_E_PLATFORM;
	}
	family = 0;
	generic_init(&request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
	status = generic_attribute(&request, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	if (status == 0) {
		status = request_send(fd, &request, request.header.nlmsg_len);
	}
	if (status == 0) {
		status = replies(fd, 1, GENL_ID_CTRL, decode_family, &family, false);
	}
	if (status == 0) {
		memset(&context, 0, sizeof(context));
		context.frequency = frequency;
		generic_init(&request, family, NL80211_CMD_GET_WIPHY, 2);
		status = generic_attribute(&request, NL80211_ATTR_WIPHY, &wiphy, sizeof(wiphy));
		if (status == 0) {
			status = generic_attribute(&request, NL80211_ATTR_SPLIT_WIPHY_DUMP, NULL, 0);
		}
		if (status == 0) {
			status = request_send(fd, &request, request.header.nlmsg_len);
		}
		if (status == 0) {
			status = replies(fd, 2, family, decode_wiphy_cb, &context, false);
		}
	}
	close(fd);
	if (status < 0 || context.radio.phy[0] == '\0') {
		return MESH_E_PLATFORM;
	}
	/* NOT inferred from a supported-iftype bit: concurrency still needs the
	 * interface-combinations/current-interface adapter before safe apply. */
	context.radio.combination_verified = false;
	*radio = context.radio;
	return MESH_OK;
}


struct empty_radio_context {
	uint32_t wiphy;
	bool occupied;
};

static int decode_interface_occupancy(const void *data, size_t length, void *context)
{
	struct empty_radio_context *radio;
	struct attributes attrs;
	const struct nlattr *attribute;
	uint32_t wiphy;
	bool found;

	if (length < GENL_HDRLEN) {
		return -1;
	}
	radio = context;
	attrs.cursor = (const unsigned char *)data + GENL_HDRLEN;
	attrs.remaining = length - GENL_HDRLEN;
	attrs.malformed = false;
	found = false;
	wiphy = 0;
	while ((attribute = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attribute) == NL80211_ATTR_WIPHY) {
			if (!attribute_u32(attribute, &wiphy)) {
				return -1;
			}
			found = true;
		}
	}
	if (attrs.malformed || !found) {
		return -1;
	}
	if (wiphy == radio->wiphy) {
		radio->occupied = true;
	}
	return 0;
}

enum mesh_error mesh_probe_empty_radio(uint32_t wiphy, uint32_t frequency,
									  struct mesh_radio *radio)
{
	struct generic_request request;
	struct empty_radio_context occupancy;
	struct mesh_radio result;
	uint16_t family;
	enum mesh_error error;
	int fd;
	int status;

	error = mesh_probe_radio(wiphy, frequency, &result);
	if (error != MESH_OK) {
		return error;
	}
	fd = netlink_socket(NETLINK_GENERIC);
	if (fd < 0) {
		return MESH_E_PLATFORM;
	}
	family = 0;
	generic_init(&request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
	status = generic_attribute(&request, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	if (status == 0) {
		status = request_send(fd, &request, request.header.nlmsg_len);
	}
	if (status == 0) {
		status = replies(fd, 1, GENL_ID_CTRL, decode_family, &family, false);
	}
	if (status == 0) {
		occupancy.wiphy = wiphy;
		occupancy.occupied = false;
		generic_init(&request, family, NL80211_CMD_GET_INTERFACE, 2);
		request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
		status = request_send(fd, &request, request.header.nlmsg_len);
		if (status == 0) {
			status = replies(fd, 2, family, decode_interface_occupancy, &occupancy, true);
		}
	}
	close(fd);
	if (status != 0) {
		return MESH_E_PLATFORM;
	}
	result.combination_verified = !occupancy.occupied;
	*radio = result;
	return MESH_OK;
}

struct wan_route_context {
	uint32_t interface_index;
	bool default_route;
};

static int decode_wan_route(const void *data, size_t length, void *context)
{
	struct wan_route_context *route;
	const struct rtmsg *message;
	struct attributes attrs;
	const struct nlattr *attribute;
	uint32_t interface_index;
	uint32_t table;

	if (length < sizeof(*message)) {
		return -1;
	}
	route = context;
	message = data;
	attrs.cursor = (const unsigned char *)data + NLMSG_ALIGN(sizeof(*message));
	attrs.remaining = length - NLMSG_ALIGN(sizeof(*message));
	attrs.malformed = false;
	interface_index = 0;
	table = message->rtm_table;
	while ((attribute = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attribute) == RTA_OIF && !attribute_u32(attribute, &interface_index)) {
			return -1;
		}
		if (attribute_type(attribute) == RTA_TABLE && !attribute_u32(attribute, &table)) {
			return -1;
		}
	}
	if (attrs.malformed) {
		return -1;
	}
	if ((message->rtm_family == AF_INET || message->rtm_family == AF_INET6) &&
		message->rtm_dst_len == 0 && message->rtm_type == RTN_UNICAST &&
		table == RT_TABLE_MAIN && interface_index == route->interface_index) {
		route->default_route = true;
	}
	return 0;
}

enum mesh_error mesh_probe_wan_role(const char *wan, enum mesh_role *role)
{
	struct {
		struct nlmsghdr header;
		struct rtmsg route;
	} request;
	struct wan_route_context context;
	int fd;
	int status;

	if (wan == NULL || role == NULL || strlen(wan) >= IF_NAMESIZE || strchr(wan, '/') != NULL) {
		return MESH_E_INPUT;
	}
	context.interface_index = if_nametoindex(wan);
	context.default_route = false;
	if (context.interface_index == 0) {
		*role = MESH_ROLE_PEER;
		return MESH_OK;
	}
	fd = netlink_socket(NETLINK_ROUTE);
	if (fd < 0) {
		return MESH_E_PLATFORM;
	}
	memset(&request, 0, sizeof(request));
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.route));
	request.header.nlmsg_type = RTM_GETROUTE;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.header.nlmsg_seq = 1;
	request.route.rtm_family = AF_UNSPEC;
	status = request_send(fd, &request, request.header.nlmsg_len);
	if (status == 0) {
		status = replies(fd, 1, RTM_NEWROUTE, decode_wan_route, &context, true);
	}
	close(fd);
	if (status != 0) {
		return MESH_E_PLATFORM;
	}
	*role = context.default_route ? MESH_ROLE_MRP : MESH_ROLE_PEER;
	return MESH_OK;
}

int mesh_decode_interface(const void *data, size_t length, struct mesh_interface *output)
{
	struct mesh_interface item;
	struct attributes attrs;
	const struct nlattr *attr;
	size_t size;
	bool have_phy = false;
	bool have_type = false;
	bool have_index = false;
	bool have_frequency = false;
	bool have_wdev = false;

	if (data == NULL || output == NULL || length < GENL_HDRLEN ||
		(uintptr_t)data % _Alignof(struct nlattr) != 0) {
		return -1;
	}
	memset(&item, 0, sizeof(item));
	attrs.cursor = (const unsigned char *)data + GENL_HDRLEN;
	attrs.remaining = length - GENL_HDRLEN;
	attrs.malformed = false;
	while ((attr = attribute_next(&attrs)) != NULL) {
		switch (attribute_type(attr)) {
		case NL80211_ATTR_WIPHY:
			if (have_phy) {
				return -1;
			}
			have_phy = attribute_u32(attr, &item.wiphy);
			if (!have_phy) {
				return -1;
			}
			break;
		case NL80211_ATTR_IFINDEX:
			if (have_index || !attribute_u32(attr, &item.ifindex)) {
				return -1;
			}
			have_index = true;
			break;
		case NL80211_ATTR_WDEV:
			if (have_wdev || attribute_length(attr) != sizeof(item.wdev)) {
				return -1;
			}
			memcpy(&item.wdev, attribute_data(attr), sizeof(item.wdev));
			have_wdev = true;
			break;
		case NL80211_ATTR_IFTYPE:
			if (have_type) {
				return -1;
			}
			have_type = attribute_u32(attr, &item.iftype);
			if (!have_type) {
				return -1;
			}
			break;
		case NL80211_ATTR_WIPHY_FREQ:
			if (have_frequency || !attribute_u32(attr, &item.frequency)) {
				return -1;
			}
			have_frequency = true;
			break;
		case NL80211_ATTR_IFNAME:
			if (item.ifname[0] != '\0') {
				return -1;
			}
			size = attribute_length(attr);
			if (size < 2 || size > sizeof(item.ifname) ||
				((const char *)attribute_data(attr))[size - 1] != '\0' ||
				memchr(attribute_data(attr), '\0', size - 1) != NULL) {
				return -1;
			}
			memcpy(item.ifname, attribute_data(attr), size);
			break;
		}
	}
	if (attrs.malformed || !have_phy || !have_type || item.iftype > NL80211_IFTYPE_MAX) {
		return -1;
	}
	if (item.ifindex != 0) {
		if (item.ifname[0] == '\0') {
			return -1;
		}
	} else if (item.ifname[0] != '\0' || item.wdev == 0 ||
			   (item.iftype != NL80211_IFTYPE_P2P_DEVICE && item.iftype != NL80211_IFTYPE_NAN)) {
		return -1;
	}
	*output = item;
	return 0;
}

static int decode_interface_list(const void *data, size_t length, void *context)
{
	struct mesh_interface_list *list = context;
	if (list->count == MESH_INTERFACES_MAX ||
		mesh_decode_interface(data, length, &list->entries[list->count]) != 0) {
		return -1;
	}
	list->count++;
	return 0;
}

enum mesh_error mesh_probe_interfaces(struct mesh_interface_list *output)
{
	struct generic_request request;
	struct mesh_interface_list result;
	uint16_t family = 0;
	int status;
	int fd;

	if (output == NULL) {
		return MESH_E_INPUT;
	}
	fd = netlink_socket(NETLINK_GENERIC);
	if (fd < 0) {
		return MESH_E_PLATFORM;
	}
	memset(&result, 0, sizeof(result));
	generic_init(&request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
	status = generic_attribute(&request, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	if (status == 0) {
		status = request_send(fd, &request, request.header.nlmsg_len);
	}
	if (status == 0) {
		status = replies(fd, 1, GENL_ID_CTRL, decode_family, &family, false);
	}
	if (status == 0) {
		generic_init(&request, family, NL80211_CMD_GET_INTERFACE, 2);
		request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
		status = request_send(fd, &request, request.header.nlmsg_len);
	}
	if (status == 0) {
		status = replies(fd, 2, family, decode_interface_list, &result, true);
	}
	close(fd);
	if (status != 0) {
		return MESH_E_PLATFORM;
	}
	*output = result;
	return MESH_OK;
}

struct ipv6_ready_context {
	uint32_t index;
	unsigned char address[16];
	bool ready;
};

static int decode_ipv6_ready(const void *data, size_t length, void *opaque)
{
	struct ipv6_ready_context *context = opaque;
	const struct ifaddrmsg *address = data;
	struct attributes attrs;
	const struct nlattr *attr;
	unsigned char value[16];
	uint32_t flags;
	bool matched = false;

	if (length < NLMSG_ALIGN(sizeof(*address))) {
		return -1;
	}
	if (address->ifa_family != AF_INET6 || address->ifa_index != context->index || address->ifa_prefixlen != 64) {
		return 0;
	}
	flags = address->ifa_flags;
	attrs.cursor = (const unsigned char *)data + NLMSG_ALIGN(sizeof(*address));
	attrs.remaining = length - NLMSG_ALIGN(sizeof(*address));
	attrs.malformed = false;
	while ((attr = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attr) == IFA_ADDRESS) {
			if (attribute_length(attr) != sizeof(value)) {
				return -1;
			}
			memcpy(value, attribute_data(attr), sizeof(value));
			matched = memcmp(value, context->address, sizeof(value)) == 0;
		} else if (attribute_type(attr) == IFA_FLAGS) {
			if (!attribute_u32(attr, &flags)) {
				return -1;
			}
		}
	}
	if (attrs.malformed) {
		return -1;
	}
	if (matched && (flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED)) == 0) {
		context->ready = true;
	}
	return 0;
}

int mesh_probe_ipv6_ready(const char *ifname, const char *address)
{
	struct {
		struct nlmsghdr header;
		struct ifaddrmsg address;
	} request;
	struct ipv6_ready_context context;
	char text[64];
	char *slash;
	int fd;
	int status;

	if (ifname == NULL || address == NULL || strlen(address) >= sizeof(text)) {
		return -1;
	}
	memset(&context, 0, sizeof(context));
	context.index = if_nametoindex(ifname);
	memcpy(text, address, strlen(address) + 1);
	slash = strchr(text, '/');
	if (slash == NULL || strcmp(slash, "/64") != 0 || context.index == 0) {
		return -1;
	}
	*slash = '\0';
	if (inet_pton(AF_INET6, text, context.address) != 1) {
		return -1;
	}
	fd = netlink_socket(NETLINK_ROUTE);
	if (fd < 0) {
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.address));
	request.header.nlmsg_type = RTM_GETADDR;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.header.nlmsg_seq = 1;
	request.address.ifa_family = AF_INET6;
	status = request_send(fd, &request, request.header.nlmsg_len);
	if (status == 0) {
		status = replies(fd, 1, RTM_NEWADDR, decode_ipv6_ready, &context, true);
	}
	close(fd);
	return status == 0 && context.ready ? 0 : -1;
}

/* Portal telemetry deliberately does not consume MPATH_INFO_METRIC: affected
 * drivers report the erroneous TEST_FRAME_LEN offset. */
#include "airtime.h"

static int decode_rate(const struct nlattr *attribute, uint32_t *rate)
{
	struct attributes attrs = attribute_children(attribute);
	const struct nlattr *item;
	uint32_t value32 = 0;
	uint16_t value16 = 0;
	bool wide = false;
	bool narrow = false;
	while ((item = attribute_next(&attrs)) != NULL) {
		if (attribute_type(item) == NL80211_RATE_INFO_BITRATE32) {
			if (wide || !attribute_u32(item, &value32)) {
				return -1;
			}
			wide = true;
		} else if (attribute_type(item) == NL80211_RATE_INFO_BITRATE) {
			if (narrow || attribute_length(item) != sizeof(value16)) {
				return -1;
			}
			memcpy(&value16, attribute_data(item), sizeof(value16));
			narrow = true;
		}
	}
	if (attrs.malformed || (!wide && !narrow)) {
		return -1;
	}
	*rate = wide ? value32 : value16;
	return *rate == 0 ? -1 : 0;
}

int mesh_decode_station(const void *data, size_t length, struct mesh_station_sample *out)
{
	struct mesh_station_sample sample = {0};
	struct attributes attrs;
	struct attributes info;
	const struct nlattr *attr;
	const struct nlattr *item;
	uint64_t seen = 0;
	bool have_info = false;
	bool packets = false;
	bool failed = false;
	bool plink = false;
	bool have_mac = false;
	bool have_index = false;
	if (data == NULL || out == NULL || length < GENL_HDRLEN ||
		(uintptr_t)data % _Alignof(struct genlmsghdr) != 0 ||
		((const struct genlmsghdr *)data)->cmd != NL80211_CMD_NEW_STATION) {
		return -1;
	}
	attrs = (struct attributes){ (const unsigned char *)data + GENL_HDRLEN, length - GENL_HDRLEN, false };
	while ((attr = attribute_next(&attrs)) != NULL) {
		if (attribute_type(attr) == NL80211_ATTR_IFINDEX) {
			if (have_index || !attribute_u32(attr, &sample.ifindex)) {
				return -1;
			}
			have_index = true;
		} else if (attribute_type(attr) == NL80211_ATTR_MAC) {
			if (have_mac || attribute_length(attr) != 6) {
				return -1;
			}
			memcpy(sample.mac, attribute_data(attr), 6);
			have_mac = true;
		} else if (attribute_type(attr) == NL80211_ATTR_STA_INFO) {
			if (have_info) {
				return -1;
			}
			have_info = true;
			info = attribute_children(attr);
			while ((item = attribute_next(&info)) != NULL) {
				unsigned int type = attribute_type(item);
				if (type < 64) {
					if ((seen & (UINT64_C(1) << type)) != 0) {
						return -1;
					}
					seen |= UINT64_C(1) << type;
				}
				if (type == NL80211_STA_INFO_TX_BITRATE) {
					if (decode_rate(item, &sample.tx_rate) != 0) {
						return -1;
					}
					sample.have_tx_rate = true;
				} else if (type == NL80211_STA_INFO_RX_BITRATE) {
					if (decode_rate(item, &sample.rx_rate) != 0) {
						return -1;
					}
					sample.have_rx_rate = true;
				} else if (type == NL80211_STA_INFO_TX_PACKETS) {
					packets = attribute_u32(item, &sample.tx_packets);
					if (!packets) {
						return -1;
					}
				} else if (type == NL80211_STA_INFO_TX_FAILED) {
					failed = attribute_u32(item, &sample.tx_failed);
					if (!failed) {
						return -1;
					}
				} else if (type == NL80211_STA_INFO_TX_RETRIES) {
					if (!attribute_u32(item, &sample.tx_retries)) {
						return -1;
					}
					sample.have_retries = true;
				} else if (type == NL80211_STA_INFO_SIGNAL_AVG) {
					int8_t signal;
					if (attribute_length(item) != 1) {
						return -1;
					}
					memcpy(&signal, attribute_data(item), 1);
					sample.signal_average = signal;
					sample.have_signal = true;
				} else if (type == NL80211_STA_INFO_PLINK_STATE) {
					uint8_t state;
					if (attribute_length(item) != 1) {
						return -1;
					}
					memcpy(&state, attribute_data(item), 1);
					sample.established = state == NL80211_PLINK_ESTAB;
					plink = true;
				}
			}
			if (info.malformed) {
				return -1;
			}
		}
	}
	sample.have_counters = packets && failed;
	if (attrs.malformed || !have_index || sample.ifindex == 0 || !have_mac || !mesh_mac_valid(sample.mac) ||
		!have_info || !plink) {
		return -1;
	}
	*out = sample;
	return 0;
}

int mesh_decode_path(const void *data, size_t length, struct mesh_path_sample *out)
{
	struct mesh_path_sample path = {0};
	struct attributes attrs;
	struct attributes info;
	const struct nlattr *attr;
	const struct nlattr *item;
	unsigned int fields = 0;
	unsigned int nested = 0;
	if (data == NULL || out == NULL || length < GENL_HDRLEN ||
		(uintptr_t)data % _Alignof(struct genlmsghdr) != 0 ||
		((const struct genlmsghdr *)data)->cmd != NL80211_CMD_NEW_MPATH) {
		return -1;
	}
	attrs = (struct attributes){ (const unsigned char *)data + GENL_HDRLEN, length - GENL_HDRLEN, false };
	while ((attr = attribute_next(&attrs)) != NULL) {
		unsigned int type = attribute_type(attr);
		unsigned int bit = type == NL80211_ATTR_IFINDEX ? 1 : type == NL80211_ATTR_MAC ? 2 :
						   type == NL80211_ATTR_MPATH_NEXT_HOP ? 4 : type == NL80211_ATTR_MPATH_INFO ? 8 : 0;
		if (bit != 0 && (fields & bit) != 0) {
			return -1;
		}
		fields |= bit;
		if (type == NL80211_ATTR_IFINDEX) {
			if (!attribute_u32(attr, &path.ifindex)) {
				return -1;
			}
		} else if (type == NL80211_ATTR_MAC || type == NL80211_ATTR_MPATH_NEXT_HOP) {
			if (attribute_length(attr) != 6) {
				return -1;
			}
			memcpy(type == NL80211_ATTR_MAC ? path.destination : path.next_hop, attribute_data(attr), 6);
		} else if (type == NL80211_ATTR_MPATH_INFO) {
			info = attribute_children(attr);
			while ((item = attribute_next(&info)) != NULL) {
				unsigned int field = attribute_type(item);
				unsigned int flag = field == NL80211_MPATH_INFO_HOP_COUNT ? 1 : field == NL80211_MPATH_INFO_FLAGS ? 2 :
									field == NL80211_MPATH_INFO_PATH_CHANGE ? 4 : 0;
				if (flag != 0 && (nested & flag) != 0) {
					return -1;
				}
				nested |= flag;
				if (field == NL80211_MPATH_INFO_HOP_COUNT || field == NL80211_MPATH_INFO_FLAGS) {
					uint8_t value;
					if (attribute_length(item) != 1) {
						return -1;
					}
					memcpy(&value, attribute_data(item), 1);
					if (field == NL80211_MPATH_INFO_HOP_COUNT) {
						path.hops = value;
					} else {
						path.active = (value & NL80211_MPATH_FLAG_ACTIVE) != 0;
					}
				} else if (field == NL80211_MPATH_INFO_PATH_CHANGE && !attribute_u32(item, &path.changes)) {
					return -1;
				}
			}
			if (info.malformed) {
				return -1;
			}
		}
	}
	if (attrs.malformed || fields != 15 || (nested & 3) != 3 || path.ifindex == 0 || path.hops == 0 ||
		!mesh_mac_valid(path.destination) || !mesh_mac_valid(path.next_hop)) {
		return -1;
	}
	*out = path;
	return 0;
}

static int decode_path_cb(const void *data, size_t length, void *context)
{
	return mesh_decode_path(data, length, context);
}

static int decode_station_cb(const void *data, size_t length, void *context)
{
	return mesh_decode_station(data, length, context);
}

int mesh_probe_path_station(uint32_t ifindex, const uint8_t destination[6],
							struct mesh_path_sample *path, struct mesh_station_sample *station)
{
	struct generic_request request;
	struct mesh_path_sample next_path = {0};
	struct mesh_station_sample next_station = {0};
	uint16_t family = 0;
	int fd;
	int result = -1;
	if (ifindex == 0 || destination == NULL || !mesh_mac_valid(destination) || path == NULL || station == NULL) {
		return -1;
	}
	fd = netlink_socket(NETLINK_GENERIC);
	if (fd < 0) {
		return -1;
	}
	generic_init(&request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
	if (generic_attribute(&request, CTRL_ATTR_FAMILY_NAME, "nl80211", 8) != 0 ||
		request_send(fd, &request, request.header.nlmsg_len) != 0 ||
		replies(fd, 1, GENL_ID_CTRL, decode_family, &family, false) != 0) {
		goto done;
	}
	generic_init(&request, family, NL80211_CMD_GET_MPATH, 2);
	if (generic_attribute(&request, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) != 0 ||
		generic_attribute(&request, NL80211_ATTR_MAC, destination, 6) != 0 ||
		request_send(fd, &request, request.header.nlmsg_len) != 0 ||
		replies(fd, 2, family, decode_path_cb, &next_path, false) != 0 || next_path.ifindex != ifindex ||
		memcmp(next_path.destination, destination, 6) != 0 || !next_path.active) {
		goto done;
	}
	generic_init(&request, family, NL80211_CMD_GET_STATION, 3);
	if (generic_attribute(&request, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) != 0 ||
		generic_attribute(&request, NL80211_ATTR_MAC, next_path.next_hop, 6) != 0 ||
		request_send(fd, &request, request.header.nlmsg_len) != 0 ||
		replies(fd, 3, family, decode_station_cb, &next_station, false) != 0 || next_station.ifindex != ifindex ||
		memcmp(next_station.mac, next_path.next_hop, 6) != 0 || !next_station.established) {
		goto done;
	}
	*path = next_path;
	*station = next_station;
	result = 0;
done:
	close(fd);
	return result;
}
