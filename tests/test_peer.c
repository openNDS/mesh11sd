/**
 * @file test_peer.c
 * @brief Host fixtures for the peer backend through an injected transport.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "peer.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int checks;
static unsigned int failures;
#define CHECK(name, value) do { \
	checks++; \
	if (value) { \
		printf("ok %u - %s\n", checks, name); \
	} else { \
		printf("not ok %u - %s\n", checks, name); \
		failures++; \
	} \
} while (0)

static void text(int root, const char *path, const char *value)
{
	char copy[256];
	size_t i;
	int fd;
	if (strlen(path) >= sizeof(copy)) {
		abort();
	}
	memcpy(copy, path, strlen(path) + 1);
	for (i = 0; copy[i] != '\0'; i++) {
		if (copy[i] != '/') {
			continue;
		}
		copy[i] = '\0';
		if (mkdirat(root, copy, 0700) != 0 && errno != EEXIST) {
			abort();
		}
		copy[i] = '/';
	}
	fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value) || close(fd) != 0) {
		abort();
	}
}

static void fixture(int root)
{
	/* Every credential and identifier in this suite is PUBLIC TEST-ONLY data. */
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '1'\n option auto_mesh_band '2g'\n option auto_mesh_key 'PUBLIC-TEST-ONLY-KEY'\n");
	text(root, "etc/config/network", "config interface 'lan'\n option device 'br-lan'\n option proto 'static'\nconfig interface 'wan'\n option device 'wan'\n");
	text(root, "etc/config/wireless", "config wifi-device 'radio0'\n option type 'mac80211'\n option phy 'phy0'\n option band '2g'\n option channel '1'\n option htmode 'HT20'\n option country 'GB'\nconfig wifi-iface 'ap0'\n option device 'radio0'\n option mode 'ap'\n option network 'lan'\n option ssid 'PUBLIC-TEST-AP'\n");
	text(root, "etc/config/dhcp", "config dhcp 'lan'\n option ignore '1'\n option start '100'\n option limit '150'\n option leasetime '1h'\n");
	text(root, "etc/config/system", "config system\n option label_macaddr '00:11:22:33:44:55'\n");
	text(root, "etc/board.json", "{\"system\":{\"label_macaddr\":\"00:11:22:33:44:55\"}}");
	text(root, "sys/class/net/br-lan/bridge/stp_state", "0\n");
}

struct fake {
	struct mesh_runtime_data service_data;
	struct mesh_native_config *config;
	uint64_t clock;
	unsigned int calls;
	unsigned int sets;
	unsigned int deletes;
	unsigned int reloads;
	unsigned int kernel_checks;
	unsigned int waits;
	unsigned int service_read_failures;
	unsigned int fail_reload;
	bool present;
	bool applied;
	bool unknown_owner;
	bool timeout_set;
	bool reject_set;
	bool bad_wpa;
	bool bad_frequency;
	bool bad_bridge;
	bool unrelated_change;
	bool foreign_radio_interface;
	bool alias_collision;
	bool v7_running;
	bool no_combinations;
	bool multiple_hw_radios;
	bool mutate_on_verify;
	bool uncertain_set;
	bool bad_instances;
	bool bad_alias;
	bool sticky_alias;
	bool frozen_clock;
	bool changed_wdev;
	bool foreign_wdev;
	volatile sig_atomic_t *cancel_on_reload;
};

static struct blob_attr *request_field(struct blob_attr *request, const char *name, int type)
{
	struct blob_attr *field;
	int remaining;
	blob_for_each_attr(field, request, remaining) {
		if (strcmp(blobmsg_name(field), name) == 0 && blobmsg_type(field) == type) {
			return field;
		}
	}
	return NULL;
}

static void must(int result)
{
	if (result != 0) {
		abort();
	}
}

static void *table(struct mesh_message *message, const char *name)
{
	void *cookie = blobmsg_open_table(&message->blob, name);
	if (cookie == NULL) {
		abort();
	}
	return cookie;
}

static void wifi_interface(struct mesh_message *message, const char *section, const char *name,
						   const char *mode, struct fake *fake)
{
	void *item = table(message, NULL);
	void *config;
	must(blobmsg_add_string(&message->blob, "section", section));
	must(blobmsg_add_string(&message->blob, "ifname", name));
	config = table(message, "config");
	must(blobmsg_add_string(&message->blob, "mode", mode));
	if (strcmp(mode, "mesh") == 0) {
		must(blobmsg_add_string(&message->blob, "mesh_id", fake->config->on_air_id));
		must(blobmsg_add_string(&message->blob, "encryption", "sae"));
	}
	blobmsg_close_table(&message->blob, config);
	blobmsg_close_table(&message->blob, item);
}

static int fake_call(void *context, const char *object, const char *method,
					 struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct fake *fake = context;
	struct mesh_message message;
	struct mesh_message copy;
	struct blob_attr *name;
	struct blob_attr *data;
	struct blob_attr *item;
	void *service;
	void *record;
	void *instances;
	int remaining;
	int result = 0;

	fake->calls++;
	must(mesh_message_init(&message));
	mesh_runtime_data_clear(reply);
	name = request_field(request, "name", BLOBMSG_TYPE_STRING);
	if (strcmp(object, "service") == 0 && strcmp(method, "list") == 0) {
		if (name == NULL) {
			abort();
		}
		if (strcmp(blobmsg_get_string(name), "mesh11sd") == 0) {
			if (fake->v7_running) {
				service = table(&message, "mesh11sd");
				instances = table(&message, "instances");
				record = table(&message, "instance1");
				must(blobmsg_add_u8(&message.blob, "running", true));
				blobmsg_close_table(&message.blob, record);
				blobmsg_close_table(&message.blob, instances);
				blobmsg_close_table(&message.blob, service);
			}
		} else if (strcmp(blobmsg_get_string(name), MESH_PEER_SERVICE) == 0) {
			if (fake->service_read_failures != 0) {
				fake->service_read_failures--;
				result = 7;
				goto done;
			}
			if (fake->present) {
				service = table(&message, MESH_PEER_SERVICE);
				must(blobmsg_add_u8(&message.blob, "autostart", false));
				if (fake->bad_instances) {
					must(blobmsg_add_string(&message.blob, "instances", "PUBLIC-TEST-WRONG-TYPE"));
				}
				record = table(&message, "data");
				blob_for_each_attr(item, (struct blob_attr *)fake->service_data.bytes, remaining) {
					if (fake->unknown_owner && strcmp(blobmsg_name(item), MESH_PEER_OWNER) == 0) {
						must(blobmsg_add_string(&message.blob, MESH_PEER_OWNER, "PUBLIC-TEST-FOREIGN-OWNER"));
					} else {
						must(blobmsg_add_blob(&message.blob, item));
					}
				}
				blobmsg_close_table(&message.blob, record);
				blobmsg_close_table(&message.blob, service);
			}
		} else {
			abort();
		}
	} else if (strcmp(object, "service") == 0 &&
			   (strcmp(method, "set") == 0 || strcmp(method, "set_data") == 0)) {
		if (name == NULL || strcmp(blobmsg_get_string(name), MESH_PEER_SERVICE) != 0) {
			abort();
		}
		if (fake->reject_set) {
			result = 6;
			goto done;
		}
		if (!fake->present) {
			item = request_field(request, "autostart", BLOBMSG_TYPE_BOOL);
			if (item == NULL || blobmsg_get_bool(item)) {
				abort();
			}
		}
		data = request_field(request, "data", BLOBMSG_TYPE_TABLE);
		if (data == NULL || mesh_field(data, MESH_PEER_OWNER, BLOBMSG_TYPE_STRING) == NULL) {
			abort();
		}
		must(mesh_message_init(&copy));
		blobmsg_for_each_attr(item, data, remaining) {
			must(blobmsg_add_blob(&copy.blob, item));
		}
		must(mesh_message_copy(&copy, &fake->service_data));
		mesh_message_clear(&copy);
		fake->present = true;
		fake->sets++;
		if (fake->uncertain_set) {
			fake->service_read_failures = 2;
		}
		if (fake->timeout_set) {
			result = 7;
		}
	} else if (strcmp(object, "service") == 0 && strcmp(method, "delete") == 0) {
		if (name == NULL || strcmp(blobmsg_get_string(name), MESH_PEER_SERVICE) != 0) {
			abort();
		}
		fake->deletes++;
		fake->present = false;
		mesh_runtime_data_clear(&fake->service_data);
	} else if (strcmp(object, "network") == 0 && strcmp(method, "reload") == 0) {
		if (blob_len(request) != 0) {
			abort();
		}
		fake->reloads++;
		if (fake->cancel_on_reload != NULL) {
			*fake->cancel_on_reload = 1;
		}
		if (fake->fail_reload != 0) {
			fake->fail_reload--;
			result = 1;
		} else {
			fake->applied = fake->present;
		}
	} else if (strcmp(object, "network.wireless") == 0 && strcmp(method, "status") == 0) {
		name = request_field(request, "device", BLOBMSG_TYPE_STRING);
		if (name == NULL || strcmp(blobmsg_get_string(name), "radio0") != 0) {
			abort();
		}
		service = table(&message, "radio0");
		must(blobmsg_add_u8(&message.blob, "up", true));
		must(blobmsg_add_u8(&message.blob, "pending", false));
		must(blobmsg_add_u8(&message.blob, "disabled", false));
		instances = blobmsg_open_array(&message.blob, "interfaces");
		if (instances == NULL) {
			abort();
		}
		wifi_interface(&message, "ap0", "phy0-ap0", "ap", fake);
		if (fake->applied) {
			wifi_interface(&message, "mesh11sd_v8_mesh", fake->config->mesh_ifname, "mesh", fake);
		}
		blobmsg_close_array(&message.blob, instances);
		blobmsg_close_table(&message.blob, service);
	} else if (strcmp(object, "wpa_supplicant") == 0 && strcmp(method, "iface_status") == 0) {
		if (name == NULL || strcmp(blobmsg_get_string(name), fake->config->mesh_ifname) != 0) {
			abort();
		}
		must(blobmsg_add_string(&message.blob, "state", fake->bad_wpa ? "SCANNING" : "COMPLETED"));
		must(blobmsg_add_u32(&message.blob, "frequency", fake->config->config.frequency_mhz));
		if (fake->mutate_on_verify) {
			fake->unknown_owner = true;
		}
	} else if (strcmp(object, "network.interface.lan") == 0 && strcmp(method, "status") == 0) {
		must(blobmsg_add_string(&message.blob, "proto", fake->applied ? "dhcp" : "static"));
		must(blobmsg_add_string(&message.blob, "device", "br-lan"));
	} else if (strcmp(object, "network.interface.mesh11sd_v8_management") == 0 && strcmp(method, "status") == 0) {
		result = fake->alias_collision || fake->applied || fake->sticky_alias ? 0 : 4;
		if (result == 0) {
			must(blobmsg_add_string(&message.blob, "proto", "static"));
			must(blobmsg_add_u8(&message.blob, "up", !fake->bad_alias));
			must(blobmsg_add_string(&message.blob, "device", "br-lan"));
		}
	} else {
		abort();
	}
done:
	if (result == 0) {
		must(mesh_message_copy(&message, reply));
	}
	mesh_message_clear(&message);
	return result;
}

static uint64_t fake_now(void *context)
{
	return ((struct fake *)context)->clock;
}

static void fake_wait(void *context, unsigned int milliseconds)
{
	struct fake *fake = context;
	fake->waits++;
	if (!fake->frozen_clock) {
		fake->clock += milliseconds;
	}
}

static enum mesh_error fake_interfaces(void *context, struct mesh_interface_list *list)
{
	struct fake *fake = context;
	struct mesh_interface *item;
	memset(list, 0, sizeof(*list));
	item = &list->entries[list->count++];
	memcpy(item->ifname, "phy0-ap0", 9);
	item->ifindex = 10;
	item->wiphy = 0;
	item->iftype = NL80211_IFTYPE_AP;
	item->frequency = 2412;
	item = &list->entries[list->count++];
	memcpy(item->ifname, "unrelated-sta", 14);
	item->ifindex = fake->unrelated_change ? 99 : 20;
	item->wiphy = 1;
	item->iftype = NL80211_IFTYPE_STATION;
	item->frequency = 5180;
	item = &list->entries[list->count++];
	item->wiphy = 1;
	item->iftype = NL80211_IFTYPE_P2P_DEVICE;
	item->wdev = fake->changed_wdev ? 43 : 42;
	if (fake->applied || fake->foreign_radio_interface || fake->foreign_wdev) {
		item = &list->entries[list->count++];
		memcpy(item->ifname, fake->foreign_radio_interface ? "foreign-mesh" : fake->config->mesh_ifname,
			   strlen(fake->foreign_radio_interface ? "foreign-mesh" : fake->config->mesh_ifname) + 1);
		item->ifindex = 30;
		item->iftype = NL80211_IFTYPE_MESH_POINT;
		item->frequency = fake->bad_frequency ? 2437 : 2412;
		if (fake->foreign_wdev) {
			memset(item->ifname, 0, sizeof(item->ifname));
			item->ifindex = 0;
			item->iftype = NL80211_IFTYPE_P2P_DEVICE;
			item->wdev = 64;
		}
	}
	return MESH_OK;
}

static enum mesh_error fake_wiphy(void *context, uint32_t index, uint32_t frequency, struct mesh_radio *radio)
{
	struct fake *fake = context;
	if (index != 0 || frequency != 2412) {
		return MESH_E_RADIO;
	}
	memset(radio, 0, sizeof(*radio));
	memcpy(radio->phy, "phy0", 5);
	radio->mesh_supported = true;
	radio->frequency_supported = true;
	radio->multiple_hw_radios = fake->multiple_hw_radios;
	if (!fake->no_combinations) {
		radio->combination_count = 1;
		radio->combinations[0].maximum = 5;
		radio->combinations[0].channels = 1;
		radio->combinations[0].limit_count = 2;
		radio->combinations[0].limits[0].types = UINT64_C(1) << NL80211_IFTYPE_AP;
		radio->combinations[0].limits[0].maximum = 4;
		radio->combinations[0].limits[1].types = UINT64_C(1) << NL80211_IFTYPE_MESH_POINT;
		radio->combinations[0].limits[1].maximum = 1;
	}
	return MESH_OK;
}

static int fake_bridge_check(void *context, const struct mesh_plan *plan)
{
	struct fake *fake = context;
	fake->kernel_checks++;
	return fake->applied && !fake->bad_bridge && strcmp(plan->bridge_ifname, "br-lan") == 0 ? 0 : -1;
}

static enum mesh_error bridge(void *context, const char *name, uint8_t mac[6])
{
	(void)context;
	if (strcmp(name, "br-lan") != 0) {
		return MESH_E_PLATFORM;
	}
	mesh_mac_parse("02:11:22:33:44:66", mac);
	return MESH_OK;
}

static enum mesh_error role(void *context, const char *wan, enum mesh_role *out)
{
	(void)context;
	(void)wan;
	*out = MESH_ROLE_PEER;
	return MESH_OK;
}

static struct mesh_peer_transport transport(struct fake *fake)
{
	struct mesh_peer_transport result = {
		.context = fake, .invoke = fake_call, .now_ms = fake_now, .wait_ms = fake_wait,
		.interfaces = fake_interfaces, .wiphy = fake_wiphy, .verify_bridge = fake_bridge_check
	};
	return result;
}

static int directory(int root, const char *name)
{
	if (mkdirat(root, name, 0700) != 0) {
		abort();
	}
	return openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

union netlink_fixture {
	struct nlattr align;
	unsigned char data[4096];
};

static size_t nla(union netlink_fixture *buffer, size_t offset, unsigned int type,
				  const void *value, size_t length)
{
	struct nlattr *attribute;
	size_t total = NLA_ALIGN(NLA_HDRLEN + length);
	if (offset + total > sizeof(buffer->data)) {
		abort();
	}
	attribute = (struct nlattr *)(buffer->data + offset);
	memset(attribute, 0, total);
	attribute->nla_type = (uint16_t)type;
	attribute->nla_len = (uint16_t)(NLA_HDRLEN + length);
	if (length != 0) {
		memcpy(buffer->data + offset + NLA_HDRLEN, value, length);
	}
	return offset + total;
}

static size_t combination_wire(union netlink_fixture *wire, uint32_t maximum, bool wrong_width)
{
	union netlink_fixture types;
	union netlink_fixture fields;
	union netlink_fixture limits;
	union netlink_fixture config;
	union netlink_fixture combinations;
	uint32_t number = 2;
	size_t size;
	size_t length;

	memset(wire, 0, sizeof(*wire));
	size = nla(&types, 0, NL80211_IFTYPE_AP, NULL, 0);
	size = nla(&types, size, NL80211_IFTYPE_MESH_POINT, NULL, 0);
	length = nla(&fields, 0, NL80211_IFACE_LIMIT_MAX, &number, sizeof(number));
	length = nla(&fields, length, NL80211_IFACE_LIMIT_TYPES | NLA_F_NESTED, types.data, size);
	size = nla(&limits, 0, 1 | NLA_F_NESTED, fields.data, length);
	length = nla(&config, 0, NL80211_IFACE_COMB_MAXNUM, &maximum, wrong_width ? 1 : sizeof(maximum));
	number = 1;
	length = nla(&config, length, NL80211_IFACE_COMB_NUM_CHANNELS, &number, sizeof(number));
	number = 25;
	length = nla(&config, length, NL80211_IFACE_COMB_BI_MIN_GCD, &number, sizeof(number));
	length = nla(&config, length, NL80211_IFACE_COMB_STA_AP_BI_MATCH, NULL, 0);
	length = nla(&config, length, NL80211_IFACE_COMB_LIMITS | NLA_F_NESTED, limits.data, size);
	size = nla(&combinations, 0, 1 | NLA_F_NESTED, config.data, length);
	length = nla(wire, GENL_HDRLEN, NL80211_ATTR_INTERFACE_COMBINATIONS | NLA_F_NESTED, combinations.data, size);
	size = nla(&types, 0, NL80211_IFTYPE_MESH_POINT, NULL, 0);
	return nla(wire, length, NL80211_ATTR_SUPPORTED_IFTYPES | NLA_F_NESTED, types.data, size);
}

static void test_wire_decoders(void)
{
	union netlink_fixture wire;
	struct mesh_radio radio = {0};
	struct mesh_radio old;
	struct mesh_interface interface = {0};
	struct mesh_interface previous;
	struct mesh_message message;
	struct mesh_runtime_data blob = {0};
	uint32_t number;
	uint32_t beacons[] = {25, 100};
	uint64_t wdev = 42;
	size_t size;
	void *nested[14];
	size_t i;
	unsigned int seed = 17;

	size = combination_wire(&wire, 2, false);
	CHECK("actual nl80211 grouped AP mesh combination wire format decoded",
		  mesh_decode_wiphy(wire.data, size, 2412, &radio) == 0 &&
		  radio.combination_count == 1 &&
		  radio.combinations[0].limit_count == 1 &&
		  radio.combinations[0].infrastructure_beacon_match &&
		  mesh_combination_allows(&radio, 1, beacons, 2));
	old = radio;
	CHECK("truncated combination wire input leaves previous evidence intact", mesh_decode_wiphy(wire.data, size - 1, 2412, &radio) < 0 && memcmp(&old, &radio, sizeof(radio)) == 0);
	size = combination_wire(&wire, 2, true);
	CHECK("combination maximum with wrong native width rejected", mesh_decode_wiphy(wire.data, size, 2412, &radio) < 0);
	size = combination_wire(&wire, 0, false);
	CHECK("zero combination maximum is not usable evidence", mesh_decode_wiphy(wire.data, size, 2412, &radio) < 0);
	size = combination_wire(&wire, 2, false);
	radio.combination_count = MESH_COMBINATIONS_MAX;
	CHECK("combination array overflow fails rather than dropping evidence", mesh_decode_wiphy(wire.data, size, 2412, &radio) < 0);
	size = GENL_HDRLEN;
	number = 0;
	size = nla(&wire, size, NL80211_ATTR_WIPHY, &number, sizeof(number));
	number = NL80211_IFTYPE_AP;
	size = nla(&wire, size, NL80211_ATTR_IFTYPE, &number, sizeof(number));
	number = 7;
	size = nla(&wire, size, NL80211_ATTR_IFINDEX, &number, sizeof(number));
	size = nla(&wire, size, NL80211_ATTR_IFNAME, "wlan0", 6);
	size = nla(&wire, size, NL80211_ATTR_WDEV, &wdev, sizeof(wdev));
	CHECK("actual netdev interface dump decodes identity and wdev",
		  mesh_decode_interface(wire.data, size, &interface) == 0 &&
		  interface.ifindex == 7 &&
		  interface.wdev == 42 &&
		  strcmp(interface.ifname, "wlan0") == 0);
	previous = interface;
	size = nla(&wire, size, NL80211_ATTR_IFINDEX, &number, sizeof(number));
	CHECK("duplicate interface identity rejected transactionally", mesh_decode_interface(wire.data, size, &interface) < 0 && memcmp(&previous, &interface, sizeof(interface)) == 0);
	size = GENL_HDRLEN;
	number = 1;
	size = nla(&wire, size, NL80211_ATTR_WIPHY, &number, sizeof(number));
	number = NL80211_IFTYPE_P2P_DEVICE;
	size = nla(&wire, size, NL80211_ATTR_IFTYPE, &number, sizeof(number));
	size = nla(&wire, size, NL80211_ATTR_WDEV, &wdev, sizeof(wdev));
	CHECK("unrelated P2P_DEVICE need not have a netdev name or index", mesh_decode_interface(wire.data, size, &interface) == 0 && interface.wdev == 42 && interface.ifindex == 0);
	((struct nlattr *)(wire.data + GENL_HDRLEN + NLA_ALIGN(NLA_HDRLEN + 4)))->nla_len = 3;
	CHECK("malformed interface attribute is not skipped", mesh_decode_interface(wire.data, size, &interface) < 0);

	must(mesh_message_init(&message));
	must(blobmsg_add_string(&message.blob, "state", "COMPLETED"));
	must(mesh_message_copy(&message, &blob));
	CHECK("typed response parser accepts valid actual-format record", mesh_reply_valid(&blob) && mesh_root_field(&blob, "state", BLOBMSG_TYPE_STRING) != NULL);
	CHECK("response lookup rejects wrong value type", mesh_root_field(&blob, "state", BLOBMSG_TYPE_INT32) == NULL);
	blob.length--;
	CHECK("truncated response is rejected before field iteration", !mesh_reply_valid(&blob));
	must(blobmsg_add_string(&message.blob, "state", "SCANNING"));
	must(mesh_message_copy(&message, &blob));
	CHECK("duplicate response keys cannot override earlier values", !mesh_reply_valid(&blob));
	mesh_message_clear(&message);
	must(mesh_message_init(&message));
	for (i = 0; i < 14; i++) {
		nested[i] = table(&message, "nested");
	}
	for (i = 14; i > 0; i--) {
		blobmsg_close_table(&message.blob, nested[i - 1]);
	}
	must(mesh_message_copy(&message, &blob));
	CHECK("deeply nested response cannot exhaust stack", !mesh_reply_valid(&blob));
	mesh_message_clear(&message);
	for (i = 0; i < 10000; i++) {
		size_t j;
		seed = seed * 1103515245U + 12345U;
		size = seed % sizeof(wire.data);
		for (j = 0; j < size; j++) {
			seed = seed * 1103515245U + 12345U;
			wire.data[j] = (unsigned char)(seed >> 16);
		}
		blob.length = size;
		memcpy(blob.bytes, wire.data, size);
		(void)mesh_reply_valid(&blob);
		(void)mesh_decode_interface(wire.data, size, &interface);
	}
	CHECK("10000 malformed ubus and interface messages complete safely", true);
	mesh_runtime_data_clear(&blob);
}

static void test_profiles_and_limits(void)
{
	struct mesh_radio radio;
	struct mesh_mobility mobility;
	uint32_t beacons[] = { 25, 100, 100, 100, 100, 100 };
	struct fake fake = {0};
	unsigned int level;
	static const unsigned int intervals[] = { 0, 0, 0, 0, 0, 0, 0 };
	static const int rssi[] = { 0, -68, -72, -75, -78, -81, -79 };
	/* v7 source 4280-4356; columns DTIM, root, RANN, PREQ, timeout, peers, retries. */
	static const unsigned int reference[7][7] = {
		{0, 0, 0, 0, 0, 0, 0},
		{0, 4, 2000, 500, 1500, 16, 4},
		{0, 4, 1500, 400, 1100, 32, 3},
		{0, 4, 1000, 350, 700, 64, 2},
		{0, 4, 500, 300, 450, 96, 1},
		{0, 4, 300, 250, 400, 96, 1},
		{1, 0, 250, 200, 350, 64, 1}
	};

	for (level = 0; level <= 6; level++) {
		if (level == 6) {
			CHECK("closed v8 item15 rejects voice PA as a mobility level", mesh_mobility_read(level, false, false, &mobility) == MESH_E_INPUT);
			CHECK("invalid voice level does not mutate last valid mobility plan", mobility.level == 5 && mobility.rootmode == 4);
			continue;
		}
		CHECK("closed v8 item15 beacon unchanged and reference connect-time RSSI", mesh_mobility_read(level, false, false, &mobility) == MESH_OK &&
			  mobility.beacon_interval == intervals[level] && mobility.rssi_threshold == rssi[level]);
		CHECK("closed v8 item15 root0 static root4 moving plus retained timing vector", mobility.level == level &&
		  mobility.dtim_period == reference[level][0] &&
		  mobility.rootmode == reference[level][1] &&
		  mobility.rann_interval == reference[level][2] &&
		  mobility.preq_min_interval == reference[level][3] &&
		  mobility.active_path_timeout == reference[level][4] &&
		  mobility.max_peer_links == reference[level][5] &&
		  mobility.max_preq_retries == reference[level][6] &&
		  mobility.tuned == (level != 0));
	}
	fake_wiphy(&fake, 0, 2412, &radio);
	CHECK("cfg80211 AP plus mesh combination with one channel", mesh_combination_allows(&radio, 1, beacons, 2));
	CHECK("cfg80211 multiple managed APs plus mesh", mesh_combination_allows(&radio, 4, beacons, 5));
	CHECK("cfg80211 aggregate maximum enforced", !mesh_combination_allows(&radio, 5, beacons, 6));
	radio.combinations[0].limits[0].maximum = 1;
	CHECK("cfg80211 per-type AP maximum enforced", !mesh_combination_allows(&radio, 2, beacons, 3));
	radio.combinations[0].limits[0].maximum = 4;
	radio.combinations[0].limits[1].types = UINT64_C(1) << NL80211_IFTYPE_STATION;
	CHECK("supported-iftype alone cannot replace mesh combination limit", !mesh_combination_allows(&radio, 1, beacons, 2));
	radio.combinations[0].limits[1].types = UINT64_C(1) << NL80211_IFTYPE_MESH_POINT;
	radio.combinations[0].beacon_min_gcd = 50;
	CHECK("kernel minimum beacon GCD enforced", !mesh_combination_allows(&radio, 1, beacons, 2));
	beacons[0] = 100;
	CHECK("explicit beacon workaround can satisfy minimum GCD", mesh_combination_allows(&radio, 1, beacons, 2));
	radio.combinations[0].infrastructure_beacon_match = true;
	beacons[0] = 25;
	radio.combinations[0].beacon_min_gcd = 0;
	CHECK("STA AP beacon equality flag not wrongly imposed on AP mesh", mesh_combination_allows(&radio, 1, beacons, 2));
	radio.combination_count = 0;
	CHECK("AP mesh without driver combination proof rejected", !mesh_combination_allows(&radio, 1, beacons, 2));
	CHECK("single mesh iftype requires no concurrency advertisement", mesh_combination_allows(&radio, 0, beacons, 1));
}

static void test_driver_evidence(int root)
{
	struct mesh_native_config config = {0};
	text(root, "sys/class/ieee80211/phy0/device/placeholder", "PUBLIC-TEST-ONLY");
	if (symlinkat("/sys/bus/pci/drivers/ath11k_pci", root, "sys/class/ieee80211/phy0/device/driver") != 0) {
		abort();
	}
	CHECK("ath11k evidence cannot reintroduce v7 mobility beacon changes",
		  mesh_native_config_read(root, &config) == MESH_OK &&
		  config.beacon_workaround &&
		  config.mobility.beacon_interval == 0 && config.radio_beacon_interval == 100);
	unlinkat(root, "sys/class/ieee80211/phy0/device/driver", 0);
	if (symlinkat("/sys/bus/platform/drivers/mt76_wmac", root, "sys/class/ieee80211/phy0/device/driver") != 0) {
		abort();
	}
	CHECK("mt76_wmac evidence leaves configured radio beacon unchanged",
		  mesh_native_config_read(root, &config) == MESH_OK &&
		  config.beacon_workaround &&
		  config.mobility.beacon_interval == 0 && config.radio_beacon_interval == 100);
	unlinkat(root, "sys/class/ieee80211/phy0/device/driver", 0);
	if (symlinkat("/sys/bus/pci/drivers/mt7915e", root, "sys/class/ieee80211/phy0/device/driver") != 0) {
		abort();
	}
	CHECK("unlisted driver does not inherit unrelated beacon workaround",
		  mesh_native_config_read(root, &config) == MESH_OK &&
		  !config.beacon_workaround &&
		  config.mobility.beacon_interval == 0 && config.radio_beacon_interval == 100);
	unlinkat(root, "sys/class/ieee80211/phy0/device/driver", 0);
	mesh_native_config_clear(&config);
}

static void append(int root, const char *path, const char *value)
{
	int fd = openat(root, path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value) || close(fd) != 0) {
		abort();
	}
}

static void test_config_guards(int root)
{
	struct mesh_native_config config = {0};
	append(root, "etc/config/network", "config interface 'mesh11sd_v8_management'\n option proto 'static'\n");
	CHECK("reserved committed management section is never overlaid", mesh_native_config_read(root, &config) == MESH_E_INPUT);
	fixture(root);
	append(root, "etc/config/wireless", "config wifi-iface 'mesh11sd_v8_mesh'\n option disabled '1'\n");
	CHECK("reserved committed mesh section is never overwritten even disabled", mesh_native_config_read(root, &config) == MESH_E_INPUT);
	fixture(root);
	text(root, "etc/config/dhcp", "config dhcp 'lan'\n option ignore '0'\n option start '100'\n option limit '150'\n option leasetime '1h'\n");
	CHECK("native DHCP evidence marks stock LAN server unsafe for peer", mesh_native_config_read(root, &config) == MESH_OK && !config.peer_services_ready);
	text(root, "etc/config/dhcp", "config dhcp 'lan'\n option ignore '1'\n option ra 'server'\n option start '100'\n option limit '150'\n option leasetime '1h'\n");
	CHECK("native RA evidence is checked separately from DHCPv4 ignore", mesh_native_config_read(root, &config) == MESH_OK && !config.peer_services_ready);
	fixture(root);
	append(root, "etc/config/mesh11sd", " option mesh_node_mobility_level '0'\n");
	text(root, "etc/config/wireless", "config wifi-device 'radio0'\n option phy 'phy0'\n option band '2g'\n option channel '1'\n option htmode 'HT20'\n option beacon_int '200'\n");
	CHECK("level zero preserves explicit wifi-device beacon interval",
		  mesh_native_config_read(root, &config) == MESH_OK &&
		  !config.mobility.tuned &&
		  config.radio_beacon_interval == 200);
	fixture(root);
	mesh_native_config_clear(&config);
}

static void test_backend(int root)
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct mesh_peer second;
	struct fake fake = { .config = &config };
	struct mesh_peer_transport io = transport(&fake);
	struct mesh_discovery_ops discovery = { .context = &peer, .radio_config = mesh_peer_radio,
										   .bridge = bridge, .role = role };
	struct mesh_runtime_data wanted = {0};
	struct mesh_runtime_data observed = {0};
	struct mesh_runtime_data empty = {0};
	struct mesh_runtime_transaction transaction = {0};
	struct stat metadata;
	enum mesh_runtime_result result;
	unsigned int count;
	volatile sig_atomic_t cancellation = 0;
	int dir = directory(root, "success");

	CHECK("native private peer session opens", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("owner token is metadata-only 0600", fstatat(dir, "owner", &metadata, AT_SYMLINK_NOFOLLOW) == 0 && (metadata.st_mode & 0777) == 0600 && metadata.st_size == 32);
	CHECK("second lifetime owner cannot acquire session", mesh_peer_open(&second, dir, &io) != 0);
	CHECK("normal configured AP mesh startup reaches tested planner", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK && config.configured_ap_count == 1);
	CHECK("native peer preflight validates namespaces", mesh_peer_preflight(&peer, &config) == 0);
	CHECK("peer payload allows proven AP coexistence", mesh_runtime_payload(&config, &plan, &wanted) == MESH_OK);
	count = fake.calls;
	CHECK("dry run never calls actual request transport",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, true, &transaction) == MESH_RUNTIME_DRY_RUN &&
		  fake.calls == count);
	CHECK("real backend constructs service set and network reload",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK &&
		  fake.sets == 1 &&
		  fake.reloads == 1);
	CHECK("post reload verifies supplicant kernel bridge and DHCP", fake.kernel_checks != 0 && fake.applied && transaction.active);
	CHECK("procd list data roundtrips canonical confidential payload", peer.ops.read(&peer, &observed) == 0 &&
		  observed.length == wanted.length &&
		  memcmp(observed.bytes, wanted.bytes, wanted.length) == 0);
	fake.bad_instances = true;
	CHECK("wrong-type instances cannot hide a foreign executable service", peer.ops.read(&peer, &observed) != 0);
	fake.bad_instances = false;
	count = fake.sets;
	CHECK("owned repeated apply is no-op without another service set",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_UNCHANGED &&
		  fake.sets == count);
	CHECK("stop deletes only private service and releases runtime mesh",
		  mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_OK &&
		  fake.deletes == 1 &&
		  !fake.present &&
		  !fake.applied);
	CHECK("unrelated PHY baseline stays intact through apply stop", peer.baseline.count == 3 && peer.baseline.entries[1].ifindex == 20);
	mesh_peer_close(&peer);
	CHECK("released session can be reacquired", mesh_peer_open(&peer, dir, &io) == 0);
	fake.no_combinations = true;
	CHECK("active AP without kernel combination proof fails", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_E_RADIO);
	fake.no_combinations = false;
	fake.foreign_radio_interface = true;
	CHECK("unmanaged interface on selected PHY is not removed or guessed", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_E_RADIO && fake.deletes == 1);
	fake.foreign_radio_interface = false;
	fake.foreign_wdev = true;
	CHECK("selected-PHY P2P device is foreign even without a netdev", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_E_RADIO);
	fake.foreign_wdev = false;
	fake.multiple_hw_radios = true;
	CHECK("multi-radio evidence does not override closed v8 no-beacon-change policy",
		  mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK &&
		  config.mobility.beacon_interval == 0 && config.radio_beacon_interval == 100);
	fake.multiple_hw_radios = false;
	CHECK("valid AP evidence can be replanned", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	config.peer_services_ready = false;
	CHECK("unsafe pre-existing DHCP or RA server is not silently left active", mesh_peer_preflight(&peer, &config) != 0);
	config.peer_services_ready = true;
	fake.v7_running = true;
	CHECK("running v7 blocks beta activation", mesh_peer_preflight(&peer, &config) != 0);
	fake.v7_running = false;
	fake.alias_collision = true;
	CHECK("foreign management alias blocks activation", mesh_peer_preflight(&peer, &config) != 0);
	fake.alias_collision = false;
	CHECK("clean namespace preflight succeeds again", mesh_peer_preflight(&peer, &config) == 0);
	text(dir, "transaction.pending", "mesh11sd-v8-transaction 1\nphase=prepared\ninput=0 inode=1 size=1 mode=100600 uid=0 gid=0 mtime=1:0\n");
	CHECK("valid interrupted metadata is atomically retained without secret backup",
		  mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_OK &&
		  fstatat(dir, "transaction.interrupted.0", &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
		  (metadata.st_mode & 0777) == 0600);
	text(dir, "transaction.pending", "mesh11sd-v8-transaction 1\nphase=prepared\ninput=0 inode=1 size=1 mode=100600 uid=0 gid=0 mtime=1:0\n");
	CHECK("second interrupted journal does not overwrite first retained metadata",
		  mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_OK &&
		  fstatat(dir, "transaction.interrupted.1", &metadata, AT_SYMLINK_NOFOLLOW) == 0);
	text(dir, "transaction.pending", "mesh11sd-v8-transaction 1\nphase=unknown\nphase=restored\n");
	CHECK("forged later phase line cannot bless malformed pending metadata",
		  mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_STALE_JOURNAL &&
		  fstatat(dir, "transaction.pending", &metadata, AT_SYMLINK_NOFOLLOW) == 0);
	text(dir, "transaction.pending", "mesh11sd-v8-transaction 1\nphase=prepared\ninput=0 inode=99999999999999999999999999999999 size=1 mode=100600 uid=0 gid=0 mtime=1:0\n");
	CHECK("overflowing journal metadata fails closed without conversion overflow", mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_STALE_JOURNAL);
	unlinkat(dir, "transaction.pending", 0);
	mesh_runtime_payload(&config, &plan, &wanted);
	mesh_runtime_transaction_clear(&transaction);
	fake.timeout_set = true;
	CHECK("timed out publish resolved by owned read-back not guessed",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	fake.timeout_set = false;
	CHECK("timeout-resolved state stops cleanly", mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_OK);
	mesh_runtime_transaction_clear(&transaction);
	fake.fail_reload = 1;
	CHECK("native reload error compensates service and netifd",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK &&
		  !fake.present &&
		  !fake.applied);
	mesh_runtime_transaction_clear(&transaction);
	fake.bad_wpa = true;
	CHECK("supplicant SCANNING cannot fake successful mesh setup",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK &&
		  !fake.present);
	fake.bad_wpa = false;
	mesh_runtime_transaction_clear(&transaction);
	fake.bad_frequency = true;
	CHECK("wrong kernel mesh frequency triggers compensation",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK);
	fake.bad_frequency = false;
	mesh_runtime_transaction_clear(&transaction);
	fake.bad_bridge = true;
	CHECK("bridge attachment or DAD failure triggers compensation",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK);
	fake.bad_bridge = false;
	mesh_runtime_transaction_clear(&transaction);
	fake.bad_alias = true;
	CHECK("management alias must be up and static after reload",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK);
	fake.bad_alias = false;
	mesh_runtime_transaction_clear(&transaction);
	fake.frozen_clock = true;
	fake.bad_wpa = true;
	count = fake.waits;
	CHECK("failed clock cannot hang post-reload verification forever",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK &&
		  fake.waits - count <= 61);
	fake.frozen_clock = false;
	fake.bad_wpa = false;
	mesh_runtime_transaction_clear(&transaction);
	peer.cancelled = &cancellation;
	fake.cancel_on_reload = &cancellation;
	CHECK("interrupt during network reload compensates owned data and objects",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, &cancellation, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK &&
		  !fake.present &&
		  !fake.applied);
	fake.cancel_on_reload = NULL;
	peer.cancelled = NULL;
	mesh_runtime_transaction_clear(&transaction);
	CHECK("cleanup-negative test starts from verified owned state",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	fake.sticky_alias = true;
	CHECK("cleanup cannot claim success while owned management alias remains", mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_ROLLBACK_NEEDED && !fake.present);
	fake.sticky_alias = false;
	CHECK("alias disappearance allows retry to complete cleanup", mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_OK);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("unrelated-wdev guard test starts from verified owned state",
		  mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	fake.changed_wdev = true;
	CHECK("unrelated non-netdev identity change is not silently accepted or deleted",
		  mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_ROLLBACK_NEEDED &&
		  fake.changed_wdev);
	fake.changed_wdev = false;
	CHECK("unchanged unrelated evidence allows guarded cleanup retry", mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_OK);
	mesh_runtime_transaction_clear(&transaction);
	fake.uncertain_set = true;
	result = mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction);
	CHECK("uncertain transport state remains explicit and retryable", result == MESH_RUNTIME_ROLLBACK_NEEDED && transaction.active && fake.present);
	fake.uncertain_set = false;
	CHECK("retry resolves uncertain owned publication safely", mesh_runtime_stop(dir, &peer.ops, &transaction) == MESH_RUNTIME_OK && !fake.present);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("withdrawal crash fixture starts from a verified peer", mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	CHECK("service withdrawal before netifd reload leaves stale runtime evidence", peer.ops.replace(&peer, &wanted, &empty) == 0 && !fake.present && fake.applied);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("restart can inspect a deleted service without adopting stale kernel names", mesh_peer_open(&peer, dir, &io) == 0 && !peer.present);
	fake.v7_running = true;
	count = fake.reloads;
	CHECK("detached recovery still refuses a running v7 service", mesh_peer_recover_detached(&peer, root, &config) == MESH_RUNTIME_CONFLICT && fake.reloads == count);
	fake.v7_running = false;
	fake.alias_collision = true;
	count = fake.deletes;
	CHECK("detached recovery does not delete an unrelated persistent alias",
		  mesh_peer_recover_detached(&peer, root, &config) == MESH_RUNTIME_ROLLBACK_NEEDED &&
		  fake.alias_collision &&
		  fake.deletes == count);
	fake.alias_collision = false;
	CHECK("recognized interrupted withdrawal restores committed netifd before planning",
		  mesh_peer_recover_detached(&peer, root, &config) == MESH_RUNTIME_OK &&
		  !fake.present &&
		  !fake.applied);
	CHECK("post-withdrawal recovery reaches normal AP mesh planning again",
		  mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK &&
		  mesh_peer_preflight(&peer, &config) == 0 &&
		  mesh_runtime_payload(&config, &plan, &wanted) == MESH_OK);
	CHECK("pre-crash native apply succeeds", mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("process restart reads retained owner marker", mesh_peer_open(&peer, dir, &io) == 0 && peer.present);
	CHECK("restart allows only its proven existing mesh interface",
		  mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK &&
		  mesh_peer_preflight(&peer, &config) == 0);
	mesh_runtime_payload(&config, &plan, &wanted);
	CHECK("crash recovery releases reconstructable owned overlay", mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_OK && !fake.present && !fake.applied);
	CHECK("post recovery startup can apply again", mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction) == MESH_RUNTIME_OK);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("second restart retains ownership without secret backups", mesh_peer_open(&peer, dir, &io) == 0);
	mesh_native_plan(root, &discovery, &config, &facts, &plan);
	mesh_peer_preflight(&peer, &config);
	mesh_runtime_payload(&config, &plan, &wanted);
	wanted.bytes[wanted.length - 4] ^= 1;
	count = fake.deletes;
	CHECK("changed reconstructed payload after crash is not overwritten",
		  mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_CONFLICT &&
		  fake.deletes == count &&
		  fake.present);
	mesh_runtime_payload(&config, &plan, &wanted);
	CHECK("unchanged reconstruction remains recoverable", mesh_peer_recover(&peer, root, &config, &plan, &wanted) == MESH_RUNTIME_OK);
	mesh_runtime_transaction_clear(&transaction);
	fake.mutate_on_verify = true;
	result = mesh_runtime_apply(root, dir, &config, &plan, &wanted, &peer.ops, NULL, false, &transaction);
	CHECK("external owner replacement after publish is preserved", result == MESH_RUNTIME_ROLLBACK_NEEDED && fake.present && fake.deletes == count + 1);
	mesh_peer_close(&peer);
	CHECK("foreign existing service owner cannot be adopted", mesh_peer_open(&peer, dir, &io) != 0 && fake.present);
	close(dir);
	mesh_runtime_data_clear(&fake.service_data);
	mesh_runtime_data_clear(&observed);
	mesh_runtime_data_clear(&wanted);
	mesh_runtime_transaction_clear(&transaction);
	mesh_native_config_clear(&config);
}

int main(int argc, char **argv)
{
	int root;
	if (argc != 2) {
		return 2;
	}
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return 2;
	}
	puts("TAP version 13");
	fixture(root);
	test_wire_decoders();
	test_profiles_and_limits();
	test_driver_evidence(root);
	test_config_guards(root);
	test_backend(root);
	close(root);
	printf("1..%u\n# %u checks, %u failures\n", checks, checks, failures);
	return failures == 0 ? 0 : 1;
}
