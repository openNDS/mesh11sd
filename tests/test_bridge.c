/**
 * @file test_bridge.c
 * @brief Host fixtures for the owned bridge parser and lifecycle.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define MESH_SERVICES_FIXTURE_ONLY
#include "test_services.c"
#include "bridge.h"
#include <dirent.h>

static const char bridge_network[] =
	"config interface 'lan'\n option device 'br-lan'\n option proto 'static'\n"
	"config device 'lan_bridge'\n option name 'br-lan'\n option type 'bridge'\n option stp '1'\n"
	" option igmp_snooping '1'\n option mtu '1500'\n list ports 'lan1'\n list ports 'lan2'\n"
	"config interface 'wan'\n option device 'wan'\n";

struct bridge_fake {
	struct service_fake services;
	struct mesh_runtime_data data;
	char owner[33];
	bool present;
	bool applied;
	bool original_present;
	bool foreign_owner;
	bool foreign_device;
	bool anchor_collision;
	bool reject_set;
	bool uncertain_set;
	unsigned int reject_after;
	unsigned int sets;
	unsigned int deletes;
	unsigned int reloads;
	unsigned int fail_reload;
	volatile sig_atomic_t *cancel_reload;
};

static void stable_text(int root, const char *path, const char *value)
{
	char contents[256];
	size_t length;
	if (mesh_read_bounded_at(root, path, contents, sizeof(contents), &length, NULL) == 0 &&
		length == strlen(value) && memcmp(contents, value, length) == 0) {
		return;
	}
	text(root, path, value);
}

static void clear_members(int root)
{
	int fd = mesh_open_at_root(root, "sys/class/net/br-lan/brif", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	DIR *dir;
	struct dirent *entry;
	if (fd < 0) {
		return;
	}
	dir = fdopendir(fd);
	if (dir == NULL) {
		abort();
	}
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] != '.') {
			must(unlinkat(fd, entry->d_name, 0));
		}
	}
	closedir(dir);
}

static void render(struct bridge_fake *fake)
{
	int root = fake->services.root;
	struct blob_attr *devices = mesh_root_field(&fake->data, "network-device", BLOBMSG_TYPE_TABLE);
	struct blob_attr *device = mesh_field(devices, "br-lan", BLOBMSG_TYPE_TABLE);
	struct blob_attr *field = mesh_field(device, "macaddr", BLOBMSG_TYPE_STRING);
	char mac[32];
	const char *address = fake->original_present ? "02:11:22:33:44:66" : "02:aa:bb:cc:dd:ee";
	bool exists = fake->present || fake->original_present;
	clear_members(root);
	unlinkat(root, "sys/class/net/lan1/master", 0);
	unlinkat(root, "sys/class/net/lan2/master", 0);
	fake->applied = fake->present;
	if (!exists) {
		unlinkat(root, "sys/class/net/br-lan/bridge/stp_state", 0);
		unlinkat(root, "sys/class/net/br-lan/bridge", AT_REMOVEDIR);
		unlinkat(root, "sys/class/net/br-lan/brif", AT_REMOVEDIR);
		unlinkat(root, "sys/class/net/br-lan/address", 0);
		unlinkat(root, "sys/class/net/br-lan", AT_REMOVEDIR);
		return;
	}
	stable_text(root, "sys/class/net/br-lan/bridge/stp_state", fake->present ? "0\n" : "1\n");
	if (fake->present && field != NULL) {
		address = blobmsg_get_string(field);
	}
	snprintf(mac, sizeof(mac), "%s\n", address);
	stable_text(root, "sys/class/net/br-lan/address", mac);
	if (mkdirat(root, "sys/class/net/br-lan/brif", 0700) != 0 && errno != EEXIST) {
		abort();
	}
	text(root, "sys/class/net/br-lan/brif/lan1", "");
	text(root, "sys/class/net/br-lan/brif/lan2", "");
	text(root, "sys/class/net/br-lan/brif/phy0-ap0", "");
	if (fake->services.peer.applied) {
		char member[128];
		snprintf(member, sizeof(member), "sys/class/net/br-lan/brif/%s", fake->services.peer.config->mesh_ifname);
		text(root, member, "");
	}
	must(symlinkat("../br-lan", root, "sys/class/net/lan1/master"));
	must(symlinkat("../br-lan", root, "sys/class/net/lan2/master"));
}

static int bridge_call(void *context, const char *object, const char *method,
					   struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct bridge_fake *fake = context;
	struct blob_attr *name = request_field(request, "name", BLOBMSG_TYPE_STRING);
	struct blob_attr *kind;
	struct blob_attr *field;
	struct mesh_message message;
	void *service;
	void *data;
	void *instance;
	int remaining;
	int result = 0;
	must(mesh_message_init(&message));
	if (strcmp(object, "service") == 0 && name != NULL && strcmp(blobmsg_get_string(name), MESH_BRIDGE_SERVICE) == 0) {
		if (strcmp(method, "list") == 0) {
			if (fake->present) {
				service = table(&message, MESH_BRIDGE_SERVICE);
				must(blobmsg_add_u8(&message.blob, "autostart", false));
				data = table(&message, "data");
				must(blobmsg_add_string(&message.blob, MESH_PEER_OWNER, fake->foreign_owner ? "PUBLIC-TEST-FOREIGN" : fake->owner));
				blob_for_each_attr(field, (struct blob_attr *)fake->data.bytes, remaining) {
					must(blobmsg_add_blob(&message.blob, field));
				}
				blobmsg_close_table(&message.blob, data);
				blobmsg_close_table(&message.blob, service);
			}
		} else if (strcmp(method, "delete") == 0) {
			fake->deletes++;
			fake->present = false;
			mesh_runtime_data_clear(&fake->data);
		} else if (strcmp(method, "set") == 0 || strcmp(method, "set_data") == 0) {
			fake->sets++;
			if (fake->reject_set || (fake->reject_after != 0 && fake->sets >= fake->reject_after)) {
				result = 7;
			} else {
				struct mesh_message normalized;
				data = request_field(request, "data", BLOBMSG_TYPE_TABLE);
				field = mesh_field(data, MESH_PEER_OWNER, BLOBMSG_TYPE_STRING);
				if (field == NULL) {
					abort();
				}
				snprintf(fake->owner, sizeof(fake->owner), "%s", blobmsg_get_string(field));
				must(mesh_message_init(&normalized));
				kind = mesh_field(data, "network-device", BLOBMSG_TYPE_TABLE);
				if (kind == NULL) {
					abort();
				}
				must(blobmsg_add_blob(&normalized.blob, kind));
				kind = mesh_field(data, "network-interface", BLOBMSG_TYPE_TABLE);
				if (kind == NULL) {
					abort();
				}
				must(blobmsg_add_blob(&normalized.blob, kind));
				must(mesh_message_copy(&normalized, &fake->data));
				mesh_message_clear(&normalized);
				fake->present = true;
				if (fake->uncertain_set) {
					fake->uncertain_set = false;
					result = 7;
				}
			}
		} else {
			abort();
		}
	} else if (strcmp(object, "service") == 0 && strcmp(method, "get_data") == 0) {
		field = request_field(request, "type", BLOBMSG_TYPE_STRING);
		if (field == NULL) {
			abort();
		}
		if (fake->present) {
			service = table(&message, MESH_BRIDGE_SERVICE);
			instance = table(&message, "*");
			kind = mesh_root_field(&fake->data, blobmsg_get_string(field), BLOBMSG_TYPE_TABLE);
			must(blobmsg_add_blob(&message.blob, kind));
			blobmsg_close_table(&message.blob, instance);
			blobmsg_close_table(&message.blob, service);
		}
		if (fake->foreign_device && strcmp(blobmsg_get_string(field), "network-device") == 0) {
			service = table(&message, "foreign-service");
			instance = table(&message, "*");
			data = table(&message, "network-device");
			{
				void *device = table(&message, "br-lan");
				must(blobmsg_add_string(&message.blob, "type", "bridge"));
				blobmsg_close_table(&message.blob, device);
			}
			blobmsg_close_table(&message.blob, data);
			blobmsg_close_table(&message.blob, instance);
			blobmsg_close_table(&message.blob, service);
		}
	} else if (strcmp(object, "network.interface." MESH_BRIDGE_ANCHOR) == 0) {
		result = fake->applied || fake->anchor_collision ? 0 : 4;
		if (result == 0) {
			must(blobmsg_add_u8(&message.blob, "up", true));
			must(blobmsg_add_string(&message.blob, "device", "br-lan"));
		}
	} else if (strcmp(object, "network.wireless") == 0 && strcmp(method, "status") == 0 &&
			   request_field(request, "device", BLOBMSG_TYPE_STRING) == NULL) {
		service = table(&message, "radio0");
		instance = blobmsg_open_array(&message.blob, "interfaces");
		data = table(&message, NULL);
		must(blobmsg_add_string(&message.blob, "ifname", "phy0-ap0"));
		{
			void *cfg = table(&message, "config");
			void *net;
			must(blobmsg_add_string(&message.blob, "mode", "ap"));
			net = blobmsg_open_array(&message.blob, "network");
			must(blobmsg_add_string(&message.blob, NULL, "lan"));
			blobmsg_close_array(&message.blob, net);
			blobmsg_close_table(&message.blob, cfg);
		}
		blobmsg_close_table(&message.blob, data);
		blobmsg_close_array(&message.blob, instance);
		blobmsg_close_table(&message.blob, service);
	} else {
		mesh_message_clear(&message);
		if (strcmp(object, "network") == 0 && strcmp(method, "reload") == 0) {
			fake->reloads++;
			if (fake->fail_reload != 0) {
				fake->fail_reload--;
				return 7;
			}
		}
		result = service_call(context, object, method, request, reply);
		if (strcmp(object, "network") == 0 && strcmp(method, "reload") == 0 && result == 0) {
			render(fake);
			if (fake->cancel_reload != NULL) {
				*fake->cancel_reload = 1;
			}
		}
		return result;
	}
	if (result == 0) {
		must(mesh_message_copy(&message, reply));
	}
	mesh_message_clear(&message);
	return result;
}

struct bridge_discovery {
	struct mesh_peer *peer;
	struct mesh_bridge *bridge;
};

static enum mesh_error radio_config_bridge(void *context, const struct mesh_native_config *config, struct mesh_radio *out)
{
	return mesh_peer_radio(((struct bridge_discovery *)context)->peer, config, out);
}

static enum mesh_error actual_bridge(void *context, const char *name, uint8_t mac[6])
{
	struct mesh_bridge *state = ((struct bridge_discovery *)context)->bridge;
	bool stp;
	return strcmp(name, state->disk.config.name) == 0 && mesh_bridge_probe(state, mac, &stp, true) == 0 ? MESH_OK : MESH_E_PLATFORM;
}

static void setup_bridge_fixture(int root, struct bridge_fake *fake, bool existing)
{
	services_fixture(root);
	text(root, "etc/config/network", bridge_network);
	text(root, "sys/class/net/br-guest/bridge/stp_state", "1\n");
	text(root, "sys/class/net/lan1/ifindex", "2\n");
	text(root, "sys/class/net/lan2/ifindex", "3\n");
	fake->services.root = root;
	fake->original_present = existing;
	render(fake);
}

static void test_owned_bridge(int root, bool existing)
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct mesh_bridge state;
	struct bridge_fake fake = { .services = { .peer.config = &config, .dns_pid = 100, .foreign_pid = 999, .lan_up = true } };
	struct mesh_peer_transport io = transport(&fake.services.peer);
	struct bridge_discovery context = { .peer = &peer, .bridge = &state };
	struct mesh_discovery_ops discovery = { .context = &context, .radio_config = radio_config_bridge,
										   .bridge = actual_bridge, .role = role };
	struct mesh_runtime_transaction transaction = {0};
	struct mesh_bridge_config parsed;
	struct mesh_file_stamp stamp;
	struct mesh_radio initial_radio;
	struct stat metadata;
	struct blob_attr *device;
	struct blob_attr *field;
	char oldmac[32];
	unsigned int count;
	volatile sig_atomic_t cancelled = 0;
	int dir;
	setup_bridge_fixture(root, &fake, existing);
	dir = directory(root, "bridge-state");
	io.invoke = bridge_call;
	io.restart_dnsmasq = service_restart;
	CHECK("bridge owner session opens", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("native committed inputs read before actual bridge exists", mesh_native_config_read(root, &config) == MESH_OK);
	CHECK("role snapshot precedes bridge transition", mesh_services_open(&peer, root, &config) == 0);
	CHECK("radio and exclusive peer prerequisites precede bridge mutation", mesh_peer_preflight(&peer, &config) == 0 && mesh_peer_radio(&peer, &config, &initial_radio) == MESH_OK);
	CHECK("committed LAN bridge ports and options parsed", mesh_bridge_config_read(root, &config, &parsed, &stamp) == 0 && parsed.port_count == 2 && parsed.option_count == 2 && parsed.stp);
	append(root, "etc/config/network", "config bridge-vlan 'advanced'\n option device 'br-lan'\n option vlan '10'\n");
	CHECK("target VLAN filtering configuration fails explicitly", mesh_bridge_config_read(root, &config, &parsed, &stamp) != 0);
	text(root, "etc/config/network", bridge_network);
	append(root, "etc/config/network", "config device 'other'\n option name 'br-guest'\n option type 'bridge'\n list ports 'lan1'\n");
	CHECK("port claimed by foreign committed bridge is never stolen", mesh_bridge_config_read(root, &config, &parsed, &stamp) != 0);
	text(root, "etc/config/network", bridge_network);
	append(root, "etc/config/network", "config interface 'wan6'\n option device 'lan2'\n");
	CHECK("other uplink interfaces cannot share a LAN member", mesh_bridge_config_read(root, &config, &parsed, &stamp) != 0);
	text(root, "etc/config/network", bridge_network);
	CHECK("same-root VLAN names without kernel proof conflict", !mesh_bridge_links_disjoint(root, "eth0.1", "eth0.2"));
	text(root, "proc/net/vlan/eth0.1", "eth0.1  VID: 1 REORDER_HDR: 1\nDevice: eth0\n");
	text(root, "proc/net/vlan/eth0.2", "eth0.2  VID: 2 REORDER_HDR: 1\nDevice: eth0\n");
	CHECK("classic LAN WAN VLANs require and accept distinct kernel VID proof", mesh_bridge_links_disjoint(root, "eth0.1", "eth0.2"));
	CHECK("physical uplink parent can never enter tagged LAN bridge", !mesh_bridge_links_disjoint(root, "eth0", "eth0.2"));
	text(root, "proc/net/vlan/eth0.2", "eth0.2  VID: 1 REORDER_HDR: 1\nDevice: eth0\n");
	CHECK("spoofed VLAN suffix does not substitute for actual kernel VID", !mesh_bridge_links_disjoint(root, "eth0.1", "eth0.2"));
	CHECK("reread captures restored committed snapshot", mesh_native_config_read(root, &config) == MESH_OK);
	config.manage_services = true;
	peer.config = &config;
	fake.foreign_device = true;
	CHECK("typed procd get_data rejects foreign runtime bridge owner", mesh_bridge_open(&state, &peer, root, &config) != 0);
	fake.foreign_device = false;
	fake.anchor_collision = true;
	CHECK("foreign runtime anchor name is never replaced", mesh_bridge_open(&state, &peer, root, &config) != 0);
	fake.anchor_collision = false;
	unlinkat(root, "sys/class/net/lan1/master", 0);
	must(symlinkat("../br-foreign", root, "sys/class/net/lan1/master"));
	CHECK("kernel foreign master blocks onboarding", mesh_bridge_open(&state, &peer, root, &config) != 0);
	unlinkat(root, "sys/class/net/lan1/master", 0);
	if (existing) {
		must(symlinkat("../br-lan", root, "sys/class/net/lan1/master"));
		text(root, "sys/class/net/br-lan/brif/unknown-eth", "");
		CHECK("unknown existing wired member is preserved by rejecting transition", mesh_bridge_open(&state, &peer, root, &config) != 0);
		unlinkat(root, "sys/class/net/br-lan/brif/unknown-eth", 0);
		text(root, "sys/class/net/br-lan/brif/.hidden-uplink", "");
		CHECK("dot-prefixed foreign bridge members are not hidden from ownership proof", mesh_bridge_open(&state, &peer, root, &config) != 0);
		unlinkat(root, "sys/class/net/br-lan/brif/.hidden-uplink", 0);
	}
	CHECK("owned bridge evidence opens without writes", mesh_bridge_open(&state, &peer, root, &config) == 0 && fake.sets == 0);
	if (!existing) {
		fake.original_present = true;
		render(&fake);
	}
	text(root, "sys/class/net/br-lan/address", "02:99:88:77:66:55\n");
	CHECK("prepublication actual bridge race cannot overwrite a new external state", mesh_bridge_apply(&state) != 0 && fake.sets == 0 && !state.journal_exists);
	fake.original_present = existing;
	render(&fake);
	CHECK("DNS suppression is staged before bridge configuration", mesh_services_prepare(&peer) == 0 && fake.services.restarts == 1);
	fake.uncertain_set = true;
	CHECK("real bridge service publication creates and pins actual bridge MAC", mesh_bridge_apply(&state) == 0 && state.disk.pinned && fake.present && fake.applied);
	CHECK("bridge verification proves STP off ports AP and private anchor", mesh_bridge_verify(&state) == 0);
	{
		char unchanged[8];
		size_t size;
		CHECK("unrelated bridge STP is never enforced off", mesh_read_bounded_at(root, "sys/class/net/br-guest/bridge/stp_state", unchanged, sizeof(unchanged), &size, NULL) == 0 && size == 2 && memcmp(unchanged, "1\n", 2) == 0);
	}
	CHECK("bridge journal is finite public snapshot with mode0600", fstatat(dir, "bridge.meta", &metadata, 0) == 0 && (metadata.st_mode & 0777) == 0600 && metadata.st_size == sizeof(struct mesh_bridge_disk));
	device = mesh_field(mesh_root_field(&fake.data, "network-device", BLOBMSG_TYPE_TABLE), "br-lan", BLOBMSG_TYPE_TABLE);
	field = mesh_field(device, "ports", BLOBMSG_TYPE_ARRAY);
	CHECK("actual procd network-device request preserves exact wired ports", field != NULL && blobmsg_check_array(field, BLOBMSG_TYPE_STRING) == 2);
	field = mesh_field(device, "igmp_snooping", BLOBMSG_TYPE_BOOL);
	CHECK("configured bridge option survives runtime STP override", field != NULL && blobmsg_get_bool(field));
	count = fake.sets;
	CHECK("repeated bridge apply has no process or config drift", mesh_bridge_apply(&state) == 0 && fake.sets == count);
	snprintf(oldmac, sizeof(oldmac), "%02x:%02x:%02x:%02x:%02x:%02x\n", state.disk.actual_mac[0], state.disk.actual_mac[1], state.disk.actual_mac[2], state.disk.actual_mac[3], state.disk.actual_mac[4], state.disk.actual_mac[5]);
	text(root, "sys/class/net/br-lan/address", "02:99:88:77:66:55\n");
	count = fake.sets;
	CHECK("external actual MAC change is conflict never silently overwritten", mesh_bridge_apply(&state) != 0 && fake.sets == count);
	text(root, "sys/class/net/br-lan/address", oldmac);
	CHECK("planner now obtains real bridge identity without manual STP gate", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	config.manage_services = true;
	CHECK("bridge IID uses observed MAC distinct from permanent factory MAC", memcmp(plan.identity.bridge_mac, state.disk.actual_mac, 6) == 0 && memcmp(plan.identity.bridge_mac, plan.identity.factory_mac, 6) != 0);
	CHECK("bridge plus role service plus peer form integrated native startup", mesh_peer_preflight(&peer, &config) == 0 && mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	CHECK("bridge cannot restore STP under a live owned peer", mesh_bridge_stop(&state) != 0 && fake.present && fake.services.peer.present);
	CHECK("active bridge proof tolerates only owned mesh and proven AP", mesh_bridge_verify(&state) == 0);
	CHECK("peer withdraws before LAN services and bridge restoration", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && !fake.services.peer.present);
	CHECK("bridge ownership can then restore original state", mesh_bridge_stop(&state) == 0 && !fake.present && !state.journal_exists);
	CHECK("clean bridge stop is idempotent", mesh_bridge_stop(&state) == 0);
	mesh_services_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("fresh role snapshot supports failure compensation", mesh_native_config_read(root, &config) == MESH_OK && mesh_services_open(&peer, root, &config) == 0);
	CHECK("fresh bridge snapshot opens", mesh_bridge_open(&state, &peer, root, &config) == 0);
	CHECK("second DNS preparation succeeds", mesh_services_prepare(&peer) == 0);
	peer.cancelled = &cancelled;
	fake.cancel_reload = &cancelled;
	CHECK("signal during bridge creation stops before mesh startup", mesh_bridge_apply(&state) != 0 && cancelled && !fake.services.peer.present);
	fake.cancel_reload = NULL;
	CHECK("role restoration remains safe with no live mesh", mesh_services_restore(&peer) == 0);
	CHECK("cancelled bridge creation compensates despite stop signal", mesh_bridge_stop(&state) == 0 && !fake.present);
	cancelled = 0;
	mesh_services_close(&peer);
	CHECK("rejected-publication bridge snapshot opens", mesh_bridge_open(&state, &peer, root, &config) == 0);
	fake.reject_set = true;
	CHECK("rejected procd publication cannot claim bridge apply", mesh_bridge_apply(&state) != 0 && !fake.present);
	fake.reject_set = false;
	CHECK("failed publication releases only its private metadata", mesh_bridge_stop(&state) == 0);
	CHECK("failed-reload bridge snapshot opens", mesh_bridge_open(&state, &peer, root, &config) == 0);
	fake.fail_reload = 1;
	CHECK("failed bootstrap reload retains recoverable owned intent", mesh_bridge_apply(&state) != 0 && fake.present && state.journal_exists);
	CHECK("failed bootstrap reload compensates to original bridge state", mesh_bridge_stop(&state) == 0 && !fake.present);
	if (!existing) {
		CHECK("absent bridge pin-publication fixture opens", mesh_bridge_open(&state, &peer, root, &config) == 0);
		fake.reject_after = fake.sets + 2;
		CHECK("pin publication failure persists actual-MAC phase2 intent", mesh_bridge_apply(&state) != 0 && state.disk.pinned && state.disk.phase == 2 && fake.present);
		fake.reject_after = 0;
		mesh_peer_close(&peer);
		mesh_bridge_clear(&state);
		CHECK("pin-publication crash reacquires the same exclusive owner", mesh_peer_open(&peer, dir, &io) == 0);
		CHECK("pin-publication crash finishes only the recognized public projection", mesh_bridge_open(&state, &peer, root, &config) == 0 && state.recovering && mesh_bridge_apply(&state) == 0);
		CHECK("recovered pin publication still restores original absent bridge", mesh_bridge_stop(&state) == 0 && !fake.present);
	}
	CHECK("new role state and peer session validate for crash fixture", mesh_native_config_read(root, &config) == MESH_OK && mesh_services_open(&peer, root, &config) == 0 && mesh_peer_preflight(&peer, &config) == 0);
	CHECK("new bridge state opens for crash fixture", mesh_bridge_open(&state, &peer, root, &config) == 0);
	CHECK("prepare crash role state", mesh_services_prepare(&peer) == 0);
	CHECK("prepare live bridge crash state", mesh_bridge_apply(&state) == 0);
	fake.present = false;
	count = fake.sets;
	CHECK("externally deleted phase2 service is not silently recreated", mesh_bridge_open(&state, &peer, root, &config) == 0 && mesh_bridge_apply(&state) != 0 && fake.sets == count);
	fake.present = true;
	CHECK("exact restored owned service is recognized after conflict", mesh_bridge_open(&state, &peer, root, &config) == 0 && mesh_bridge_verify(&state) == 0);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_bridge_clear(&state);
	CHECK("crash process reacquires same owner token", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("crash input and role evidence reconstructed", mesh_native_config_read(root, &config) == MESH_OK && mesh_services_open(&peer, root, &config) == 0 && mesh_peer_preflight(&peer, &config) == 0);
	CHECK("bridge crash snapshot adopts only exact owned public projection", mesh_bridge_open(&state, &peer, root, &config) == 0 && state.recovering);
	count = fake.sets;
	CHECK("already pinned bridge recovery performs no republish", mesh_bridge_apply(&state) == 0 && fake.sets == count);
	CHECK("crash cleanup restores original role snapshot", mesh_services_restore(&peer) == 0);
	fake.fail_reload = 1;
	CHECK("withdrawal interruption retains phase3 recovery snapshot", mesh_bridge_stop(&state) != 0 && state.disk.phase == 3 && !fake.present && state.journal_exists);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_bridge_clear(&state);
	CHECK("withdrawal crash process reacquires ownership", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("withdrawal crash metadata opens with service absent", mesh_bridge_open(&state, &peer, root, &config) == 0 && state.disk.phase == 3);
	CHECK("withdrawal crash finishes committed reload and releases own anchor", mesh_bridge_stop(&state) == 0 && !state.journal_exists);
	CHECK("changed-input fixture opens fresh bridge owner", mesh_bridge_open(&state, &peer, root, &config) == 0);
	CHECK("changed-input fixture applies native bridge", mesh_bridge_apply(&state) == 0);
	text(root, "sys/class/net/br-lan/address", "02:99:88:77:66:55\n");
	CHECK("cleanup preserves externally changed actual bridge resources", mesh_bridge_stop(&state) != 0 && fake.present);
	snprintf(oldmac, sizeof(oldmac), "%02x:%02x:%02x:%02x:%02x:%02x\n", state.disk.actual_mac[0], state.disk.actual_mac[1], state.disk.actual_mac[2], state.disk.actual_mac[3], state.disk.actual_mac[4], state.disk.actual_mac[5]);
	text(root, "sys/class/net/br-lan/address", oldmac);
	append(root, "etc/config/network", "# PUBLIC-TEST-ONLY external committed change\n");
	CHECK("committed change prevents new bridge apply", mesh_bridge_apply(&state) != 0);
	CHECK("committed change still permits release of exclusively owned objects", mesh_bridge_stop(&state) == 0 && state.released_changed_inputs && !fake.present);
	text(dir, "bridge.pending", "PUBLIC-TEST-ONLY incomplete snapshot");
	count = fake.sets;
	CHECK("unknown interrupted bridge snapshot blocks without overwriting", mesh_bridge_open(&state, &peer, root, &config) != 0 && fake.sets == count);
	unlinkat(dir, "bridge.pending", 0);
	text(dir, "bridge.meta", "PUBLIC-TEST-ONLY malformed metadata");
	CHECK("malformed bridge journal is rejected without public effects", mesh_bridge_open(&state, &peer, root, &config) != 0 && fake.sets == count);
	unlinkat(dir, "bridge.meta", 0);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_bridge_clear(&state);
	mesh_native_config_clear(&config);
	close(dir);
}

int main(int argc, char **argv)
{
	int root;
	int existing;
	int absent;
	if (argc != 2) {
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return 2;
	}
	existing = directory(root, "existing-bridge");
	absent = directory(root, "absent-bridge");
	test_owned_bridge(existing, true);
	test_owned_bridge(absent, false);
	close(existing);
	close(absent);
	close(root);
	printf("%u bridge checks; %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
