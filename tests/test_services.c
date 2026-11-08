/**
 * @file test_services.c
 * @brief Host fixtures for LAN role-service handoff and crash compensation.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

/* Reuse the already-tested public AP/mesh/netifd fixture helpers, not a live
 * bus. Existing 109 peer checks are built/run separately and unchanged. */
#define main peer_fixture_baseline_main
#include "test_peer.c"
#undef main
#include "services.h"

struct service_fake {
	struct fake peer;
	int root;
	uint32_t dns_pid;
	uint32_t foreign_pid;
	unsigned int restarts;
	unsigned int alias_starts;
	unsigned int mode_sets;
	unsigned int fail_restart;
	unsigned int fail_restart_after_change;
	unsigned int explicit_ups;
	bool manual_down;
	bool dns_crashloop;
	bool dns_restarting;
	bool dhcp4_stuck;
	bool ipv4_method_missing;
	bool ipv4_method_error;
	unsigned int modes[4];
	char owner[33];
	bool lan_up;
	bool runtime_lan;
	bool client_up;
	bool client_not_armed;
	bool ra_stuck;
	bool dhcp6_stuck;
	bool wrong_data_shape;
	bool unsafe_order;
	bool foreign_data;
	bool missing_mount;
	bool netns;
};

static bool held(struct service_fake *fake)
{
	struct blob_attr *type = mesh_root_field(&fake->peer.service_data, "wifi-iface", BLOBMSG_TYPE_TABLE);
	struct blob_attr *radio = mesh_field(type, "radio0", BLOBMSG_TYPE_TABLE);
	struct blob_attr *vif = mesh_field(radio, "mesh11sd_v8_mesh", BLOBMSG_TYPE_TABLE);
	struct blob_attr *config = mesh_field(vif, "config", BLOBMSG_TYPE_TABLE);
	struct blob_attr *disabled = mesh_field(config, "disabled", BLOBMSG_TYPE_BOOL);
	return disabled != NULL && blobmsg_get_bool(disabled);
}

static void dns_instance(struct mesh_message *message, const char *name, uint32_t process, struct service_fake *fake)
{
	void *instance = table(message, name);
	void *command;
	char path[128];
	must(blobmsg_add_u8(&message->blob, "running", true));
	must(blobmsg_add_u32(&message->blob, "pid", process));
	command = blobmsg_open_array(&message->blob, "command");
	snprintf(path, sizeof(path), "/var/etc/dnsmasq.conf.%s", name);
	must(blobmsg_add_string(&message->blob, NULL, "/usr/sbin/dnsmasq"));
	must(blobmsg_add_string(&message->blob, NULL, "-C"));
	must(blobmsg_add_string(&message->blob, NULL, path));
	must(blobmsg_add_string(&message->blob, NULL, "-k"));
	blobmsg_close_array(&message->blob, command);
	{
		void *jail = table(message, "jail");
		must(blobmsg_add_u8(&message->blob, "netns", fake->netns));
		blobmsg_close_table(&message->blob, jail);
		jail = table(message, "mount");
		if (!fake->missing_mount) {
			must(blobmsg_add_string(&message->blob, "/tmp/dnsmasq.cfg1.d", "0"));
		}
		blobmsg_close_table(&message->blob, jail);
	}
	blobmsg_close_table(&message->blob, instance);
}

static int service_call(void *context, const char *object, const char *method,
						struct blob_attr *request, struct mesh_runtime_data *reply)
{
	static const char *const modes[] = { NULL, "disabled", "server", "relay", "hybrid" };
	struct service_fake *fake = context;
	struct mesh_message message;
	struct blob_attr *field;
	void *data;
	void *service;
	size_t i;
	int result = 0;
	field = request_field(request, "name", BLOBMSG_TYPE_STRING);
	must(mesh_message_init(&message));
	if (strcmp(object, "service") == 0 && strcmp(method, "list") == 0 && field != NULL &&
		strcmp(blobmsg_get_string(field), "dnsmasq") == 0) {
		if (fake->dns_crashloop && fake->dns_restarting) {
			fake->dns_pid++;
		}
		service = table(&message, "dnsmasq");
		data = table(&message, "instances");
		dns_instance(&message, "cfg1", fake->dns_pid, fake);
		dns_instance(&message, "unrelated", fake->foreign_pid, fake);
		blobmsg_close_table(&message.blob, data);
		blobmsg_close_table(&message.blob, service);
	} else if (strcmp(object, "network.interface.lan") == 0) {
		if (strcmp(method, "status") == 0) {
			must(blobmsg_add_string(&message.blob, "proto", fake->runtime_lan ? "none" : "static"));
			must(blobmsg_add_string(&message.blob, "device", "br-lan"));
			must(blobmsg_add_u8(&message.blob, "up", fake->lan_up));
			must(blobmsg_add_u8(&message.blob, "pending", false));
			data = table(&message, "data");
			for (i = 0; i < 4; i++) {
				if (fake->modes[i] != 0) {
					must(blobmsg_add_string(&message.blob, mesh_service_modes[i], modes[fake->modes[i]]));
				}
			}
			if (fake->owner[0] != '\0') {
				must(blobmsg_add_string(&message.blob, "mesh11sd-v8-services", fake->owner));
			}
			if (fake->foreign_data) {
				must(blobmsg_add_string(&message.blob, "foreign", "PUBLIC-TEST-ONLY"));
			}
			must(blobmsg_add_string(&message.blob, "zone", "lan"));
			blobmsg_close_table(&message.blob, data);
		} else if (strcmp(method, "set_data") == 0) {
			fake->mode_sets++;
			fake->wrong_data_shape = request_field(request, "data", -1) != NULL;
			for (i = 0; i < 4; i++) {
				field = request_field(request, mesh_service_modes[i], BLOBMSG_TYPE_STRING);
				if (field != NULL) {
					unsigned int j;
					for (j = 1; j <= 4; j++) {
						if (strcmp(blobmsg_get_string(field), modes[j]) == 0) {
							fake->modes[i] = j;
						}
					}
				}
			}
			field = request_field(request, "mesh11sd-v8-services", BLOBMSG_TYPE_STRING);
			if (field != NULL) {
				snprintf(fake->owner, sizeof(fake->owner), "%s", blobmsg_get_string(field));
			}
		} else if (strcmp(method, "down") == 0) {
			fake->unsafe_order = fake->unsafe_order || fake->peer.applied;
			fake->lan_up = false;
			fake->manual_down = true;
			memset(fake->modes, 0, sizeof(fake->modes));
			memset(fake->owner, 0, sizeof(fake->owner));
		} else if (strcmp(method, "up") == 0) {
			fake->explicit_ups++;
			fake->manual_down = false;
			fake->lan_up = true;
		} else {
			abort();
		}
	} else if (strcmp(object, "dhcp") == 0) {
		bool ra = strcmp(method, "ipv6ra") == 0;
		bool v4 = strcmp(method, "ipv4leases") == 0;
		if (v4 && (fake->ipv4_method_missing || fake->ipv4_method_error)) {
			mesh_message_clear(&message);
			return fake->ipv4_method_missing ? 3 : 7;
		}
		service = table(&message, ra ? "interfaces" : "device");
		if ((ra ? fake->ra_stuck : v4 ? fake->dhcp4_stuck : fake->dhcp6_stuck) || fake->modes[ra ? 0 : v4 ? 1 : 2] != 1) {
			if (ra) {
				data = blobmsg_open_array(&message.blob, "br-lan");
				blobmsg_close_array(&message.blob, data);
			} else {
				void *leases;
				data = table(&message, "br-lan");
				leases = blobmsg_open_array(&message.blob, "leases");
				blobmsg_close_array(&message.blob, leases);
				blobmsg_close_table(&message.blob, data);
			}
		}
		blobmsg_close_table(&message.blob, service);
	} else if (strcmp(object, "network.interface.mesh11sd_v8_client") == 0) {
		if (!fake->peer.present) {
			result = 4;
		} else if (strcmp(method, "status") == 0) {
			must(blobmsg_add_string(&message.blob, "proto", "dhcp"));
			must(blobmsg_add_string(&message.blob, "device", "br-lan"));
			must(blobmsg_add_u8(&message.blob, "autostart", fake->client_up && !fake->client_not_armed));
		} else if (strcmp(method, "up") == 0) {
			fake->alias_starts++;
			fake->client_up = true;
			fake->unsafe_order = fake->unsafe_order || !fake->runtime_lan || !fake->lan_up ||
								 fake->owner[0] == '\0' || fake->modes[0] != 1 || fake->modes[2] != 1;
		} else {
			abort();
		}
	} else if (strcmp(object, "network.interface.mesh11sd_v8_management") == 0) {
		result = fake->peer.present ? 0 : 4;
		if (result == 0) {
			must(blobmsg_add_string(&message.blob, "proto", "static"));
			must(blobmsg_add_string(&message.blob, "device", "br-lan"));
			must(blobmsg_add_u8(&message.blob, "up", true));
		}
	} else {
		mesh_message_clear(&message);
		result = fake_call(context, object, method, request, reply);
		if (strcmp(object, "network") == 0 && strcmp(method, "reload") == 0 && result == 0) {
			if (fake->runtime_lan != fake->peer.present) {
				memset(fake->modes, 0, sizeof(fake->modes));
				memset(fake->owner, 0, sizeof(fake->owner));
			}
			fake->runtime_lan = fake->peer.present;
			/* netifd preserves a manual down/autostart=false across reload
			 * when the committed auto setting itself is unchanged. */
			fake->lan_up = !fake->manual_down;
			fake->peer.applied = fake->peer.present && !held(fake);
			if (!fake->peer.present) {
				fake->client_up = false;
			}
			if (fake->peer.applied && (!fake->client_up || fake->owner[0] == '\0')) {
				fake->unsafe_order = true;
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

static int service_restart(void *context, const char *instance)
{
	struct service_fake *fake = context;
	fake->restarts++;
	if (strcmp(instance, "cfg1") != 0) {
		abort();
	}
	if (fake->fail_restart != 0) {
		fake->fail_restart--;
		return -1;
	}
	fake->dns_pid++;
	fake->dns_restarting = true;
	if (fake->fail_restart_after_change != 0) {
		fake->fail_restart_after_change--;
		return -1;
	}
	return 0;
}

static void services_fixture(int root)
{
	fixture(root);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dnsmasq 'unrelated'\nconfig dhcp 'lan'\n option interface 'lan'\n option instance 'cfg1'\n option start '100'\n option limit '150'\n option leasetime '1h'\n option ra 'server'\n option dhcpv6 'server'\n");
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/dnsmasq.cfg1.d\n");
	must(mkdirat(root, "tmp", 0700));
	must(mkdirat(root, "tmp/dnsmasq.cfg1.d", 0700));
}

#ifdef MESH_SERVICES_FIXTURE_ONLY
int services_fixture_baseline_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct service_fake fake = { .peer.config = &config, .dns_pid = 100, .foreign_pid = 999, .lan_up = true };
	struct mesh_peer_transport io = transport(&fake.peer);
	struct mesh_discovery_ops discovery = { .context = &peer, .radio_config = mesh_peer_radio, .bridge = bridge, .role = role };
	struct mesh_runtime_transaction transaction = {0};
	struct mesh_runtime_data payload = {0};
	struct stat metadata;
	char include[128];
	char original_dhcp[4096];
	size_t original_length;
	int root;
	int dir;
	unsigned int count;
	volatile sig_atomic_t cancelled = 1;
	if (argc != 2) {
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return 2;
	}
	fake.root = root;
	services_fixture(root);
	must(mesh_read_bounded_at(root, "etc/config/dhcp", original_dhcp, sizeof(original_dhcp) - 1, &original_length, NULL));
	original_dhcp[original_length] = '\0';
	append(root, "etc/config/dhcp", "config odhcpd 'odhcpd'\n option maindhcp '1'\n");
	CHECK("committed main DHCPv4 owner evidence is recovered natively", mesh_native_config_read(root, &config) == MESH_OK && config.odhcpd_maindhcp);
	mesh_native_config_clear(&config);
	text(root, "etc/config/dhcp", original_dhcp);
	append(root, "etc/config/dhcp", "config odhcpd 'odhcpd'\n option maindhcp '0'\n");
	CHECK("stock ipv6-only ownership remains distinct from main DHCPv4", mesh_native_config_read(root, &config) == MESH_OK && !config.odhcpd_maindhcp);
	mesh_native_config_clear(&config);
	text(root, "etc/config/dhcp", original_dhcp);
	append(root, "etc/config/dhcp", "config odhcpd 'odhcpd'\n option maindhcp 'PUBLIC-TEST-ONLY-invalid'\n");
	CHECK("malformed main DHCP ownership evidence fails closed", mesh_native_config_read(root, &config) != MESH_OK);
	mesh_native_config_clear(&config);
	text(root, "etc/config/dhcp", original_dhcp);
	dir = directory(root, "role-state");
	io.invoke = service_call;
	io.restart_dnsmasq = service_restart;
	CHECK("owned service session opens with injected native transport", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("stock LAN server config reaches planner", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK && !config.peer_services_ready);
	CHECK("explicit DHCP pool instance selects only actual current owner", config.dns_suppression_needed && config.dns_instance_count == 1 && strcmp(config.dns_instances[0], "cfg1") == 0);
	fake.foreign_data = true;
	CHECK("unknown stored interface data is not flushed for a handoff", mesh_services_open(&peer, root, &config) != 0);
	fake.foreign_data = false;
	text(root, "var/etc/dnsmasq.conf.cfg1", "# PUBLIC-TEST-ONLY no confdir\n");
	CHECK("missing generated include directory fails without service action", mesh_services_open(&peer, root, &config) != 0 && fake.restarts == 0);
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/dnsmasq.cfg1.d\nconf-dir=/tmp/dnsmasq.cfg1.d\n");
	CHECK("ambiguous duplicate confdir rejected", mesh_services_open(&peer, root, &config) != 0);
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/dnsmasq.cfg1.d,*.other\n");
	CHECK("filter excluding owned conf is rejected", mesh_services_open(&peer, root, &config) != 0);
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/../etc\n");
	CHECK("generated path traversal cannot escape private fixture root", mesh_services_open(&peer, root, &config) != 0);
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/dnsmasq.cfg1.d\n");
	must(fchmodat(root, "tmp/dnsmasq.cfg1.d", 0777, 0));
	CHECK("unsafe writable include directory is never used", mesh_services_open(&peer, root, &config) != 0);
	must(fchmodat(root, "tmp/dnsmasq.cfg1.d", 0700, 0));
	fake.dns_pid = 0;
	CHECK("stopped current dnsmasq owner is not falsely treated as applied", mesh_services_open(&peer, root, &config) != 0);
	fake.dns_pid = 100;
	fake.missing_mount = true;
	CHECK("jailed dnsmasq without include mount cannot claim suppression", mesh_services_open(&peer, root, &config) != 0);
	fake.missing_mount = false;
	fake.netns = true;
	CHECK("different DNS network namespace is an explicit evidence failure", mesh_services_open(&peer, root, &config) != 0);
	fake.netns = false;
	CHECK("public finite mode snapshot and dnsmasq owner discovery succeed", mesh_services_open(&peer, root, &config) == 0);
	CHECK("active local servers no longer impose manual ignore prerequisites", mesh_peer_preflight(&peer, &config) == 0);
	CHECK("cancel before start creates no role journal or DNS restart", mesh_peer_managed_apply(&peer, root, &config, &plan, &cancelled, &transaction) == MESH_RUNTIME_CANCELLED && fake.restarts == 0 && fstatat(dir, "services.meta", &metadata, 0) != 0);
	fake.modes[0] = 2;
	CHECK("external mode update before handoff is preserved without restart", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && fake.restarts == 0 && fake.modes[0] == 2);
	fake.modes[0] = 0;
	CHECK("stock DHCP and RA handoff reaches actual peer backend", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	CHECK("one private DHCP alias starts only after LAN server suppression", fake.alias_starts == 1 && fake.client_up && !fake.unsafe_order);
	CHECK("mode request uses real flat netifd set_data API", !fake.wrong_data_shape && fake.mode_sets == 1);
	CHECK("dnsmasq restart is current-instance only with unrelated PID unchanged", fake.restarts == 1 && fake.foreign_pid == 999);
	CHECK("mode-only recovery metadata is private and bounded", fstatat(dir, "services.meta", &metadata, 0) == 0 && (metadata.st_mode & 0777) == 0600 && metadata.st_size == sizeof(struct mesh_services_disk));
	snprintf(include, sizeof(include), "tmp/dnsmasq.cfg1.d/mesh11sd-v8-%s.conf", peer.owner);
	CHECK("owned DHCP suppression include is 0600", fstatat(root, include, &metadata, 0) == 0 && (metadata.st_mode & 0777) == 0600);
	CHECK("active mode and both odhcpd server lists read back quiet", mesh_services_verify(&peer) == 0);
	count = fake.restarts;
	CHECK("repeat apply is idempotent without restarting services or client", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_UNCHANGED && fake.restarts == count && fake.alias_starts == 1);
	fake.client_not_armed = true;
	CHECK("unarmed DHCP alias is not falsely accepted as applied", peer.ops.verify(&peer, &plan) != 0);
	fake.client_not_armed = false;
	CHECK("armed native DHCP alias can await lease without fake lease success", peer.ops.verify(&peer, &plan) == 0);
	fake.ra_stuck = true;
	CHECK("empty RA-server array still means active, never accepted", mesh_services_verify(&peer) != 0);
	fake.ra_stuck = false;
	fake.dhcp6_stuck = true;
	CHECK("empty DHCPv6-server device array still means active", mesh_services_verify(&peer) != 0);
	fake.dhcp6_stuck = false;
	fake.dhcp4_stuck = true;
	CHECK("empty IPv4 leases table still proves an active odhcpd server", mesh_services_verify(&peer) != 0);
	fake.dhcp4_stuck = false;
	fake.ipv4_method_missing = true;
	CHECK("verified ipv6-only method absence is supported when not main DHCPv4", mesh_services_verify(&peer) == 0);
	config.odhcpd_maindhcp = true;
	CHECK("main DHCPv4 configuration requires actual IPv4 suppression evidence", mesh_services_verify(&peer) != 0);
	config.odhcpd_maindhcp = false;
	fake.ipv4_method_missing = false;
	fake.ipv4_method_error = true;
	CHECK("IPv4 lease query errors are not silently treated as method absence", mesh_services_verify(&peer) != 0);
	fake.ipv4_method_error = false;
	text(root, include, "# PUBLIC-TEST-ONLY foreign change\n");
	CHECK("externally modified private include is never silently repaired", mesh_services_verify(&peer) != 0);
	text(root, include, "# mesh11sd-v8 owned runtime suppression\nno-dhcp-interface=br-lan\n");
	fake.foreign_pid++;
	CHECK("unrelated service restart is detected, not overwritten", mesh_services_verify(&peer) != 0);
	fake.foreign_pid--;
	text(root, "var/etc/dnsmasq.conf.cfg1", "conf-dir=/tmp/dnsmasq.cfg1.d\n# PUBLIC-TEST-ONLY generation\n");
	CHECK("generated config changed under same PID is a conflict", mesh_services_verify(&peer) != 0);
	fake.dns_pid++;
	CHECK("normal platform owner regeneration accepts same command and confdir", mesh_services_verify(&peer) == 0);
	fake.dns_crashloop = true;
	fake.dns_restarting = true;
	CHECK("asynchronous platform PID churn also cannot bypass stability guard", mesh_services_verify(&peer) != 0);
	fake.dns_crashloop = false;
	CHECK("stable platform replacement can be adopted after churn ceases", mesh_services_verify(&peer) == 0);
	fake.foreign_data = true;
	CHECK("new foreign mode data prevents destructive down but withdraws mesh", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_ROLLBACK_NEEDED && fake.peer.present && !fake.peer.applied && fake.foreign_data);
	fake.foreign_data = false;
	CHECK("ordered stop holds mesh before flushing modes and restores stock services", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && !fake.peer.present && !fake.peer.applied && !fake.client_up && !fake.unsafe_order);
	CHECK("clean stop explicitly restores prior UP despite netifd autostart latch", fake.lan_up && !fake.manual_down && fake.explicit_ups > 0);
	CHECK("absent original mode fields restored absent, no null tombstones", fake.owner[0] == '\0' && fake.modes[0] == 0 && fake.modes[2] == 0);
	CHECK("cleanup removes own include and role journal only", fstatat(root, include, &metadata, 0) != 0 && fstatat(dir, "services.meta", &metadata, 0) != 0 && fake.foreign_pid == 999);
	CHECK("repeat stop is harmless", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_UNCHANGED);
	mesh_runtime_transaction_clear(&transaction);
	fake.modes[2] = 2;
	mesh_services_close(&peer);
	CHECK("fresh snapshot retains explicit original mode override", mesh_services_open(&peer, root, &config) == 0 && peer.services->disk.modes[2] == 2);
	CHECK("second managed start works after clean withdrawal", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	CHECK("explicit original mode override restored exactly", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && fake.modes[2] == 2 && fake.modes[0] == 0);
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	fake.modes[2] = 0;
	CHECK("new start snapshot succeeds", mesh_services_open(&peer, root, &config) == 0);
	fake.ra_stuck = true;
	CHECK("RA suppression failure compensates without enabling mesh", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.peer.present && !fake.peer.applied && !fake.unsafe_order);
	fake.ra_stuck = false;
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	CHECK("restart-failure snapshot succeeds", mesh_services_open(&peer, root, &config) == 0);
	fake.fail_restart = 1;
	CHECK("uncertain dnsmasq restart is read-back failure with safe compensation", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.peer.present && fstatat(root, include, &metadata, 0) != 0);
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	CHECK("crash test snapshot succeeds", mesh_services_open(&peer, root, &config) == 0);
	CHECK("crash test starts native role projection", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("new owner process reacquires retained identity lock", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("restart loads only finite old mode journal", mesh_services_open(&peer, root, &config) == 0 && peer.services->recovering);
	CHECK("restart re-detects AP mesh evidence and factory identity", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	config.manage_services = true;
	CHECK("owned active radio preflight reconstructs safe baseline", mesh_peer_preflight(&peer, &config) == 0);
	CHECK("same-config crash recovery orders mesh withdrawal and role restoration", mesh_peer_recover_managed(&peer, root, &config, &plan) == MESH_RUNTIME_OK && !fake.peer.present && fake.owner[0] == '\0');
	mesh_services_close(&peer);
	CHECK("held-crash snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	CHECK("held-crash test starts native role projection", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	config.hold_mesh = true;
	CHECK("crash after held transition is reconstructable", mesh_runtime_payload(&config, &plan, &payload) == MESH_OK && mesh_runtime_promote(root, dir, &config, &plan, &payload, &peer.ops, &transaction) == MESH_RUNTIME_OK);
	CHECK("down intent persists before clearing owned mode data", mesh_services_down(&peer) == 0 && peer.services->disk.phase == 3 && fake.owner[0] == '\0');
	config.hold_mesh = false;
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("held-crash process recovers owner and role snapshot", mesh_peer_open(&peer, dir, &io) == 0 && mesh_services_open(&peer, root, &config) == 0);
	CHECK("held-crash fresh discovery succeeds", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	config.manage_services = true;
	CHECK("held-crash preflight succeeds", mesh_peer_preflight(&peer, &config) == 0);
	CHECK("crash after data flush restores original absence exactly", mesh_peer_recover_managed(&peer, root, &config, &plan) == MESH_RUNTIME_OK && !fake.peer.present && fake.owner[0] == '\0');
	CHECK("crash after scoped down restores original UP, not merely mode fields", fake.lan_up && !fake.manual_down);
	mesh_services_close(&peer);
	fake.lan_up = false;
	fake.manual_down = true;
	count = fake.explicit_ups;
	CHECK("originally DOWN LAN takes a truthful snapshot", mesh_services_open(&peer, root, &config) == 0 && !peer.services->disk.was_up);
	CHECK("originally DOWN preparation and restore do not force LAN up", mesh_services_prepare(&peer) == 0 && mesh_services_restore(&peer) == 0 && !fake.lan_up && fake.manual_down && fake.explicit_ups == count);
	mesh_services_close(&peer);
	fake.lan_up = true;
	fake.manual_down = false;
	CHECK("restart-return failure snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	fake.fail_restart_after_change = 1;
	CHECK("failed restart remains failure even when it changed to a running PID", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.peer.present);
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	CHECK("crashloop fixture snapshot opens before PID churn", mesh_services_open(&peer, root, &config) == 0);
	fake.dns_crashloop = true;
	fake.dns_restarting = false;
	CHECK("transient running PIDs cannot claim a stable dnsmasq replacement", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.peer.applied);
	fake.dns_crashloop = false;
	CHECK("stopped crashloop permits explicit safe cleanup retry", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) != MESH_RUNTIME_ROLLBACK_NEEDED);
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	CHECK("changed-input test takes fresh snapshot", mesh_services_open(&peer, root, &config) == 0);
	CHECK("changed-input test starts native role projection", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	append(root, "etc/config/system", "# PUBLIC-TEST-ONLY externally changed input\n");
	CHECK("repeat apply rejects changed committed input", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_CONFLICT);
	CHECK("owned withdrawal remains possible after committed input changed", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && !fake.peer.present);
	mesh_runtime_transaction_clear(&transaction);
	mesh_services_close(&peer);
	CHECK("stale pre-start snapshot opens without writes", mesh_services_open(&peer, root, &config) == 0);
	count = fake.restarts;
	CHECK("changed inputs prevent service effects even before first role journal", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && fake.restarts == count && fstatat(dir, "services.meta", &metadata, 0) != 0);
	mesh_services_close(&peer);
	text(dir, "services.pending", "PUBLIC-TEST-ONLY interrupted data");
	count = fake.restarts;
	CHECK("unknown interrupted journal blocks without modifying any service", mesh_services_open(&peer, root, &config) != 0 && fake.restarts == count);
	unlinkat(dir, "services.pending", 0);
	mesh_services_close(&peer);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dnsmasq 'unrelated'\nconfig dhcp 'lan'\n option start '100'\n option limit '150'\n option leasetime '1h'\n");
	CHECK("unbound LAN pool selects every enabled dnsmasq owner", mesh_native_config_read(root, &config) == MESH_OK && config.dns_instance_count == 2);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dnsmasq 'unrelated'\nconfig dhcp 'lan'\n option start '100'\n option limit '150'\n option leasetime '1h'\n option instance 'cfg1'\nconfig dhcp 'second'\n option interface 'lan'\n option instance 'unrelated'\n");
	CHECK("multiple LAN-bound pools aggregate distinct service owners", mesh_native_config_read(root, &config) == MESH_OK && config.dns_instance_count == 2);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_native_config_clear(&config);
	mesh_runtime_data_clear(&payload);
	close(dir);
	close(root);
	printf("%u service checks; %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
