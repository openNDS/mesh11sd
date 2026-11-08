/**
 * @file test_mrp.c
 * @brief Host fixtures for the explicit IPv4-only MRP requests and recovery.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define MESH_SERVICES_FIXTURE_ONLY
#include "test_services.c"
#include "mrp.h"

struct mrp_fake {
	struct service_fake services;
	unsigned int firewall_reloads;
	unsigned int probes;
	unsigned int fail_firewall;
	unsigned int fail_portal_restart;
	unsigned int portal_restarts;
	bool firewall_active;
	bool wrong_device;
	bool wrong_target;
	bool extra_constraint;
	bool missing_family;
	bool wan_down;
	bool bad_uplink;
	bool overlapping_wan;
	bool overlapping_guest;
	bool no_route;
	bool no_dns;
	bool fail_probe;
};

static const char firewall_config[] =
	"config defaults\n option input 'REJECT'\n option forward 'REJECT'\n"
	"config zone\n option name 'lan'\n list network 'lan'\n"
	"config zone\n option name 'wan'\n list network 'wan'\n list network 'wan6'\n";

static void iptext(uint32_t ip, char text[32])
{
	snprintf(text, 32, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static int mrp_call(void *context, const char *object, const char *method,
					 struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct mrp_fake *fake = context;
	struct mesh_message message;
	void *array;
	void *entry;
	char address[32];
	bool wan = strcmp(object, "network.interface.wan") == 0 && strcmp(method, "status") == 0;
	bool gateway = strcmp(object, "network.interface.mesh11sd_v8_client") == 0 && strcmp(method, "status") == 0 && fake->services.peer.present;
	if (strcmp(object, "network.interface") == 0 && strcmp(method, "dump") == 0) {
		must(mesh_message_init(&message));
		array = blobmsg_open_array(&message.blob, "interface");
		entry = table(&message, NULL);
		must(blobmsg_add_string(&message.blob, "interface", "guest"));
		{
			void *addresses = blobmsg_open_array(&message.blob, "ipv4-address");
			void *address_entry = table(&message, NULL);
			iptext(fake->overlapping_guest ? fake->services.peer.config->config.portal_ipv4.address : 0xc6336401, address);
			must(blobmsg_add_string(&message.blob, "address", address));
			must(blobmsg_add_u32(&message.blob, "mask", 24));
			blobmsg_close_table(&message.blob, address_entry);
			blobmsg_close_array(&message.blob, addresses);
		}
		blobmsg_close_table(&message.blob, entry);
		blobmsg_close_array(&message.blob, array);
		must(mesh_message_copy(&message, reply));
		mesh_message_clear(&message);
		return 0;
	}
	if (!wan && !gateway) {
		return service_call(context, object, method, request, reply);
	}
	must(mesh_message_init(&message));
	must(blobmsg_add_u8(&message.blob, "up", wan ? !fake->wan_down : fake->services.client_up));
	must(blobmsg_add_string(&message.blob, "device", wan ? "wan" : "br-lan"));
	if (wan) {
		must(blobmsg_add_string(&message.blob, "l3_device", fake->bad_uplink ? "wan;bad" : "wan"));
		array = blobmsg_open_array(&message.blob, "ipv4-address");
		entry = table(&message, NULL);
		iptext(fake->overlapping_wan ? fake->services.peer.config->config.portal_ipv4.address : 0xc0000202, address);
		must(blobmsg_add_string(&message.blob, "address", address));
		must(blobmsg_add_u32(&message.blob, "mask", 24));
		blobmsg_close_table(&message.blob, entry);
		blobmsg_close_array(&message.blob, array);
		array = blobmsg_open_array(&message.blob, "route");
		if (!fake->no_route) {
			entry = table(&message, NULL);
			must(blobmsg_add_string(&message.blob, "target", "0.0.0.0"));
			must(blobmsg_add_u32(&message.blob, "mask", 0));
			blobmsg_close_table(&message.blob, entry);
		}
		blobmsg_close_array(&message.blob, array);
		array = blobmsg_open_array(&message.blob, "dns-server");
		if (!fake->no_dns) {
			must(blobmsg_add_string(&message.blob, NULL, "192.0.2.53"));
		}
		blobmsg_close_array(&message.blob, array);
	} else {
		must(blobmsg_add_string(&message.blob, "proto", "static"));
		must(blobmsg_add_u8(&message.blob, "autostart", fake->services.client_up));
		array = blobmsg_open_array(&message.blob, "ipv4-address");
		entry = table(&message, NULL);
		iptext(fake->services.peer.config->config.portal_ipv4.address, address);
		must(blobmsg_add_string(&message.blob, "address", address));
		must(blobmsg_add_u32(&message.blob, "mask", fake->services.peer.config->config.portal_ipv4.prefix));
		blobmsg_close_table(&message.blob, entry);
		blobmsg_close_array(&message.blob, array);
	}
	must(mesh_message_copy(&message, reply));
	mesh_message_clear(&message);
	return 0;
}

static int mrp_reload(void *context)
{
	struct mrp_fake *fake = context;
	fake->firewall_reloads++;
	if (fake->fail_firewall != 0) {
		fake->fail_firewall--;
		return -1;
	}
	fake->firewall_active = fake->services.peer.present;
	return 0;
}

static void match_string(struct mesh_message *message, const char *kind, const char *key,
						  const char *field, const char *right)
{
	void *expression = table(message, NULL);
	void *match = table(message, "match");
	void *left;
	void *descriptor;
	must(blobmsg_add_string(&message->blob, "op", "=="));
	left = table(message, "left");
	descriptor = table(message, kind);
	must(blobmsg_add_string(&message->blob, key, field));
	blobmsg_close_table(&message->blob, descriptor);
	blobmsg_close_table(&message->blob, left);
	must(blobmsg_add_string(&message->blob, "right", right));
	blobmsg_close_table(&message->blob, match);
	blobmsg_close_table(&message->blob, expression);
}

static void prefix_match(struct mesh_message *message, const struct mesh_ipv4 *pool)
{
	void *expression = table(message, NULL);
	void *match = table(message, "match");
	void *left;
	void *descriptor;
	void *right;
	char subnet[32];
	iptext(pool->address & (UINT32_MAX << (32 - pool->prefix)), subnet);
	must(blobmsg_add_string(&message->blob, "op", "=="));
	left = table(message, "left");
	descriptor = table(message, "payload");
	must(blobmsg_add_string(&message->blob, "protocol", "ip"));
	must(blobmsg_add_string(&message->blob, "field", "saddr"));
	blobmsg_close_table(&message->blob, descriptor);
	blobmsg_close_table(&message->blob, left);
	right = table(message, "right");
	descriptor = table(message, "prefix");
	must(blobmsg_add_string(&message->blob, "addr", subnet));
	must(blobmsg_add_u32(&message->blob, "len", pool->prefix));
	blobmsg_close_table(&message->blob, descriptor);
	blobmsg_close_table(&message->blob, right);
	blobmsg_close_table(&message->blob, match);
	blobmsg_close_table(&message->blob, expression);
}

static void port_match(struct mesh_message *message, const char *proto, unsigned int port)
{
	void *expression = table(message, NULL);
	void *match = table(message, "match");
	void *left;
	void *descriptor;
	must(blobmsg_add_string(&message->blob, "op", "=="));
	left = table(message, "left");
	descriptor = table(message, "payload");
	must(blobmsg_add_string(&message->blob, "protocol", proto));
	must(blobmsg_add_string(&message->blob, "field", "dport"));
	blobmsg_close_table(&message->blob, descriptor);
	blobmsg_close_table(&message->blob, left);
	must(blobmsg_add_u32(&message->blob, "right", port));
	blobmsg_close_table(&message->blob, match);
	blobmsg_close_table(&message->blob, expression);
}

static int mrp_chain(void *context, const char *chain, struct mesh_runtime_data *reply)
{
	struct mrp_fake *fake = context;
	struct mesh_message message;
	void *array;
	void *entry;
	void *rule;
	void *expressions;
	void *expression;
	void *target;
	char comment[128];
	unsigned int i;
	unsigned int j;
	must(mesh_message_init(&message));
	array = blobmsg_open_array(&message.blob, "nftables");
	for (i = 0; fake->firewall_active && i < 4; i++) {
		if (strcmp(chain, i == 0 ? "forward_lan" : i == 1 ? "srcnat_wan" : "input_lan") != 0) {
			continue;
		}
		for (j = 0; j < (i == 2 ? 2U : 1U); j++) {
			entry = table(&message, NULL);
			rule = table(&message, "rule");
			must(blobmsg_add_string(&message.blob, "family", "inet"));
			must(blobmsg_add_string(&message.blob, "table", "fw4"));
			must(blobmsg_add_string(&message.blob, "chain", chain));
			snprintf(comment, sizeof(comment), "!fw4: ubus:%s[*] %s %u", MESH_PEER_SERVICE, i == 1 ? "nat" : "rule", i);
			must(blobmsg_add_string(&message.blob, "comment", comment));
			expressions = blobmsg_open_array(&message.blob, "expr");
			match_string(&message, "meta", "key", i < 2 ? "oifname" : "iifname", fake->wrong_device ? "FOREIGN" : i < 2 ? "wan" : "br-lan");
			if (i < 2) {
				prefix_match(&message, &fake->services.peer.config->config.portal_ipv4);
			} else {
				if (!fake->missing_family) {
					match_string(&message, "meta", "key", "nfproto", "ipv4");
				}
				port_match(&message, i == 2 && j == 0 ? "tcp" : "udp", i == 2 ? 53 : 67);
			}
			if (fake->extra_constraint) {
				match_string(&message, "meta", "key", "mark", "FOREIGN");
			}
			expression = table(&message, NULL);
			if (i == 0) {
				target = table(&message, "jump");
				must(blobmsg_add_string(&message.blob, "target", fake->wrong_target ? "drop" : "accept_to_wan"));
				blobmsg_close_table(&message.blob, target);
			} else {
				must(blobmsg_add_field(&message.blob, BLOBMSG_TYPE_UNSPEC, i == 1 ? "masquerade" : "accept", NULL, 0));
			}
			blobmsg_close_table(&message.blob, expression);
			blobmsg_close_array(&message.blob, expressions);
			blobmsg_close_table(&message.blob, rule);
			blobmsg_close_table(&message.blob, entry);
		}
	}
	blobmsg_close_array(&message.blob, array);
	must(mesh_message_copy(&message, reply));
	mesh_message_clear(&message);
	return 0;
}

static int mrp_probe(void *context, const char *bridge_name, const char *uplink,
					  uint32_t address, uint8_t prefix, uint32_t process)
{
	struct mrp_fake *fake = context;
	fake->probes++;
	return !fake->fail_probe && strcmp(bridge_name, "br-lan") == 0 && strcmp(uplink, "wan") == 0 &&
		   address == fake->services.peer.config->config.portal_ipv4.address && prefix == 24 &&
		   process == fake->services.dns_pid && fake->services.client_up ? 0 : -1;
}

static int mrp_restart(void *context, const char *instance)
{
	struct mrp_fake *fake = context;
	char path[256];
	char text_buffer[2048];
	size_t size;
	snprintf(path, sizeof(path), "tmp/dnsmasq.cfg1.d/mesh11sd-v8-%s.conf", fake->services.owner);
	if (mesh_read_bounded_at(fake->services.root, path, text_buffer, sizeof(text_buffer) - 1, &size, NULL) == 0) {
		text_buffer[size] = '\0';
		if (strstr(text_buffer, "dhcp-authoritative") != NULL) {
			fake->portal_restarts++;
			if (fake->fail_portal_restart != 0) {
				fake->fail_portal_restart--;
				return -1;
			}
		}
	}
	return service_restart(context, instance);
}

static enum mesh_error mrp_role(void *context, const char *wan, enum mesh_role *out)
{
	(void)context;
	(void)wan;
	*out = MESH_ROLE_MRP;
	return MESH_OK;
}

static void dns_packets(void)
{
	static const unsigned char question[] = {
		19, 'm','e','s','h','1','1','s','d','-','v','8','-','g','a','t','e','w','a','y',
		7, 'i','n','v','a','l','i','d', 0, 0, 1, 0, 1
	};
	unsigned char response[12 + sizeof(question) + 16] = { 0x12,0x34,0x85,0x80,0,1,0,1 };
	size_t offset = 12 + sizeof(question);
	size_t i;
	memcpy(response + 12, question, sizeof(question));
	memcpy(response + offset, (unsigned char[]){0xc0,12,0,1,0,1,0,0,0,30,0,4,192,0,2,1},16);
	CHECK("native DNS parser validates owned marker A response", mesh_mrp_dns_response(response, sizeof(response), 0x1234, 0xc0000201) == 0);
	CHECK("DNS transaction mismatch cannot verify another responder", mesh_mrp_dns_response(response, sizeof(response), 0x1235, 0xc0000201) != 0);
	CHECK("DNS wrong gateway cannot claim portal readiness", mesh_mrp_dns_response(response, sizeof(response), 0x1234, 0xc0000202) != 0);
	for (i = 0; i < sizeof(response); i++) {
		if (mesh_mrp_dns_response(response, i, 0x1234, 0xc0000201) == 0) {
			break;
		}
	}
	CHECK("every truncated native DNS response rejected", i == sizeof(response));
	{
		unsigned char corrupted[sizeof(response)];
		uint32_t random = 0x31415926;
		for (i = 0; i < 10000; i++) {
			random = random * 1664525U + 1013904223U;
			memcpy(corrupted, response, sizeof(response));
			corrupted[random % sizeof(corrupted)] ^= (unsigned char)(random >> 24);
			mesh_mrp_dns_response(corrupted, (random >> 8) % (sizeof(corrupted) + 1), 0x1234, 0xc0000201);
		}
		CHECK("10000 bounded mutated DNS reply fixtures complete", true);
	}
	response[2] |= 2;
	CHECK("truncated DNS transport flag is not accepted as complete proof", mesh_mrp_dns_response(response, sizeof(response), 0x1234, 0xc0000201) != 0);
}

int main(int argc, char **argv)
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct mrp_fake fake = { .services = { .peer.config = &config, .dns_pid = 100, .foreign_pid = 999, .lan_up = true } };
	struct mesh_peer_transport io = transport(&fake.services.peer);
	struct mesh_discovery_ops discovery = { .context = &peer, .radio_config = mesh_peer_radio, .bridge = bridge, .role = mrp_role };
	struct mesh_runtime_transaction transaction = {0};
	struct mesh_runtime_data payload = {0};
	struct blob_attr *interfaces;
	struct blob_attr *gateway;
	struct blob_attr *field;
	struct stat metadata;
	char include[256];
	char snippet[2048];
	char changed[4096];
	int root;
	int dir;
	unsigned int count;
	size_t length;
	uint64_t socket_inode;
	struct mesh_runtime_data held_payload = {0};
	volatile sig_atomic_t cancelled = 1;
	if (argc != 2) {
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return 2;
	}
	fake.services.root = root;
	services_fixture(root);
	text(root, "etc/config/firewall", firewall_config);
	text(root, "proc/sys/net/ipv4/ip_forward", "1\n");
	dir = directory(root, "mrp-state");
	io.invoke = mrp_call;
	io.restart_dnsmasq = mrp_restart;
	io.reload_firewall = mrp_reload;
	io.firewall_chain = mrp_chain;
	io.portal_probe = mrp_probe;
	CHECK("MRP takes the existing exclusive owner lock", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("MRP planner derives shared IPv4 and portal root4", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK && plan.role == MESH_ROLE_MRP && config.mobility.rootmode == 4);
	CHECK("unspecified IPv6 portal profile is explicit unsupported", mesh_mrp_configure(root, &peer, &config, MESH_MRP_UNSPECIFIED) == -2 && fake.firewall_reloads == 0);
	CHECK("unretrieved GUA profile cannot emit a guessed RA policy", mesh_mrp_configure(root, &peer, &config, MESH_MRP_GUA_UNIMPLEMENTED) == -2);
	CHECK("unimplemented ULA RA profile does not claim server support", mesh_mrp_configure(root, &peer, &config, MESH_MRP_ULA_RA_UNIMPLEMENTED) == -2);
	fake.bad_uplink = true;
	CHECK("untrusted WAN interface strings cannot enter runtime requests", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.bad_uplink = false;
	text(root, "proc/sys/net/ipv4/ip_forward", "0\n");
	CHECK("disabled global IPv4 forwarding is not silently overwritten", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "proc/sys/net/ipv4/ip_forward", "1\n");
	text(root, "tmp/.uci/firewall", "PUBLIC-TEST-ONLY uncommitted firewall delta");
	CHECK("uncommitted firewall drafts cannot be accidentally activated", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	unlinkat(root, "tmp/.uci/firewall", 0);
	fake.overlapping_wan = true;
	CHECK("gateway subnet cannot collide with actual routed WAN addressing", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.overlapping_wan = false;
	fake.overlapping_guest = true;
	CHECK("foreign guest subnet is preserved by refusing overlapping portal allocation", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.overlapping_guest = false;
	fake.wan_down = true;
	CHECK("down WAN refuses portal effects", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.wan_down = false;
	fake.no_route = true;
	CHECK("missing actual WAN default route refuses portal effects", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.no_route = false;
	fake.no_dns = true;
	CHECK("missing WAN resolver evidence is not DNS success", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	fake.no_dns = false;
	append(root, "etc/config/firewall", "config zone\n option name 'foreign'\n list network 'lan'\n");
	CHECK("ambiguous zone ownership refuses public rule publication", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/firewall", firewall_config);
	append(root, "etc/config/dhcp", "config dhcp 'guest'\n option interface 'guest'\n option instance 'cfg1'\n");
	CHECK("instance-wide authoritative cannot change a foreign pool", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dhcp 'lan'\n option instance 'cfg1'\n option ignore '1'\n");
	CHECK("explicit disabled DHCP override is preserved not falsely enabled", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dhcp 'lan'\n option instance 'cfg1'\n list dhcp_option '42,192.0.2.42'\n");
	CHECK("unimplemented custom IPv4 options are rejected not silently dropped", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\n option noping '1'\nconfig dhcp 'lan'\n option instance 'cfg1'\n");
	CHECK("configured no-ping is an explicit portal conflict", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\n option sequential_ip '1'\nconfig dhcp 'lan'\n option instance 'cfg1'\n");
	CHECK("configured sequential allocation cannot bypass shared client-MAC policy", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0);
	text(root, "etc/config/dhcp", "config dnsmasq 'cfg1'\nconfig dnsmasq 'unrelated'\nconfig dhcp 'lan'\n option interface 'lan'\n option instance 'cfg1'\n option start '100'\n option limit '150'\n option leasetime '1h'\n option ra 'server'\n option dhcpv6 'server'\n");
	CHECK("reread preserves public shared pool fixture", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	CHECK("explicit IPv4-only profile configures verified existing platform owners", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) == 0 && config.dns_instance_count == 1 && strcmp(config.mrp.uplink,"wan") == 0);
	count = config.stamp_count;
	CHECK("repeated portal evidence does not grow or drift configuration", mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) == 0 && config.stamp_count == count);
	CHECK("MRP runtime emits real static gateway and fw4 service data", mesh_runtime_payload(&config, &plan, &payload) == MESH_OK);
	interfaces = mesh_root_field(&payload, "network-interface", BLOBMSG_TYPE_TABLE);
	gateway = mesh_field(interfaces, "mesh11sd_v8_client", BLOBMSG_TYPE_TABLE);
	field = mesh_field(gateway, "proto", BLOBMSG_TYPE_STRING);
	CHECK("MRP never starts a DHCP client on its gateway alias", field != NULL && strcmp(blobmsg_get_string(field), "static") == 0);
	field = mesh_field(gateway, "ipaddr", BLOBMSG_TYPE_ARRAY);
	CHECK("owned gateway addressing is represented for actual netifd", field != NULL && blobmsg_check_array(field, BLOBMSG_TYPE_STRING) == 1);
	CHECK("runtime firewall uses standard fw4 rules not loop rules", mesh_root_field(&payload, "firewall", BLOBMSG_TYPE_ARRAY) != NULL && mesh_field(gateway, "gateway", -1) == NULL);
	gateway = mesh_field(interfaces, "mesh11sd_v8_management", BLOBMSG_TYPE_TABLE);
	field = mesh_field(gateway, "ip6addr", BLOBMSG_TYPE_ARRAY);
	CHECK("MRP claims agreed per-factory portal ULA alongside actual-bridge management IID", field != NULL && blobmsg_check_array(field, BLOBMSG_TYPE_STRING) == 2);
	CHECK("original role services snapshot opens before gateway apply", mesh_services_open(&peer, root, &config) == 0 && mesh_peer_preflight(&peer, &config) == 0);
	CHECK("cancelled MRP makes no firewall DNS or network changes", mesh_peer_managed_apply(&peer, root, &config, &plan, &cancelled, &transaction) == MESH_RUNTIME_CANCELLED && fake.firewall_reloads == 0 && fake.services.restarts == 0);
	CHECK("integrated MRP startup verifies gateway firewall DHCP DNS and held ordering", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK && fake.services.peer.applied && fake.probes > 0 && !fake.services.unsafe_order);
	CHECK("IPv4-only profile emits no RA DHCPv6 or NDP service", fake.services.modes[0] == 1 && fake.services.modes[2] == 1 && fake.services.modes[3] == 1);
	snprintf(include, sizeof(include), "tmp/dnsmasq.cfg1.d/mesh11sd-v8-%s.conf", peer.owner);
	CHECK("authoritative pool include has private0600 ownership", fstatat(root, include, &metadata, 0) == 0 && (metadata.st_mode & 0777) == 0600);
	CHECK("public server snippet is bounded and readable for fixture assertions", mesh_read_bounded_at(root, include, snippet, sizeof(snippet)-1, &length, NULL) == 0);
	snippet[length] = '\0';
	text(root, "syntax-mrp.conf", snippet);
	text(root, "syntax-suppression.conf", "# PUBLIC-TEST-ONLY generated suppression fixture\nno-dhcp-interface=br-lan\n");
	CHECK("pool uses dnsmasq probing and client-MAC allocation without factory partition", strstr(snippet,"dhcp-authoritative\n") != NULL && strstr(snippet,"dhcp-range=set:mesh11sd_v8,") != NULL && strstr(snippet,"no-ping") == NULL && strstr(snippet,"sequential") == NULL && strstr(snippet,"no-dhcp-interface") == NULL);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "no-ping  \n");
	CHECK("effective foreign no-ping directive cannot disable allocation probing", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "no-ping # PUBLIC-TEST-ONLY inline comment\n");
	CHECK("inline comments cannot conceal disabled DHCP probing", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "dhcp-range = 192.0.2.10,192.0.2.20,60s\n");
	CHECK("spaces around option assignment cannot conceal a foreign pool", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "no-dhcp-interface=\"br-lan\"\n");
	CHECK("quoted interface exclusions fail closed instead of hiding disabled service", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "no-dhcp-interface=br-*\n");
	CHECK("wildcard interface exclusions cannot bypass target protection", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "interface=lo\n");
	CHECK("loopback-only listener evidence cannot advertise LAN DHCP readiness", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "interface=lo\ninterface=br-lan\nno-dhcp-interface=wan\n");
	CHECK("canonical LAN whitelist and excluded WAN remain practical native inputs", mesh_mrp_dns_guard(peer.services) == 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "dhcp-ignore=tag:mesh11sd_v8\n");
	CHECK("foreign ignore tag cannot silently disable the owned common pool", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "conf-script=/PUBLIC-TEST-ONLY/never-executed\n");
	CHECK("dynamic configuration scripts cannot escape bounded effective-option audit", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "conf-file=/etc/dnsmasq.portal-test.conf\n");
	text(root, "etc/dnsmasq.portal-test.conf", "dhcp-sequential-ip\n");
	CHECK("nested standard DNS includes cannot bypass client-MAC allocation guard", mesh_mrp_dns_guard(peer.services) != 0);
	text(root, "etc/dnsmasq.portal-test.conf", "server=192.0.2.53\ndhcp-host=02:00:00:00:00:42,192.0.2.42\n");
	CHECK("normal nested DNS configuration and static reservations remain permitted", mesh_mrp_dns_guard(peer.services) == 0);
	text(root, "etc/dnsmasq.portal-test.conf", "dhcp-host=tag:lan,02:00:00:00:00:42,192.0.2.42\n");
	CHECK("conditional legacy LAN-tag reservations are explicit unsupported, not silently changed", mesh_mrp_dns_guard(peer.services) != 0);
	unlinkat(root, "tmp/dnsmasq.cfg1.d/foreign.conf", 0);
	unlinkat(root, "etc/dnsmasq.portal-test.conf", 0);
	count = fake.firewall_reloads;
	CHECK("idempotent MRP start makes no repeated reload or pool change", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_UNCHANGED && fake.firewall_reloads == count);
	fake.wrong_device = true;
	CHECK("matching NFT comment on wrong interface is not forwarding proof", mesh_mrp_firewall_verify(&peer, false) != 0);
	fake.wrong_device = false;
	fake.wrong_target = true;
	CHECK("wrong active NFT verdict cannot claim working forwarding", mesh_mrp_firewall_verify(&peer, false) != 0);
	fake.wrong_target = false;
	fake.extra_constraint = true;
	CHECK("extra active firewall constraint is rejected rather than ignored", mesh_mrp_firewall_verify(&peer, false) != 0);
	fake.extra_constraint = false;
	fake.missing_family = true;
	CHECK("owned IPv4-only input rules cannot silently broaden into IPv6", mesh_mrp_firewall_verify(&peer, false) != 0);
	fake.missing_family = false;
	fake.fail_probe = true;
	CHECK("missing kernel socket DNS or route proof prevents MRP success", mesh_mrp_verify(&peer) != 0);
	fake.fail_probe = false;
	snprintf(changed, sizeof(changed), "%s# PUBLIC-TEST-ONLY external replacement\n", snippet);
	text(root, include, changed);
	CHECK("foreign DNS include mutation is preserved as a conflict", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) != MESH_RUNTIME_OK && fake.services.peer.present);
	text(root, include, snippet);
	CHECK("ordered portal stop withdraws DHCP gateway and only owned fw4 rules", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && !fake.firewall_active && !fake.services.peer.present && fake.services.foreign_pid == 999);
	mesh_services_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	text(root, "tmp/dnsmasq.cfg1.d/foreign.conf", "no-ping # PUBLIC-TEST-ONLY preactivation conflict\n");
	CHECK("suppressed portal preactivation snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	count = fake.portal_restarts;
	CHECK("hidden unsafe allocator settings fail before any authoritative server restart", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && fake.portal_restarts == count && !fake.services.peer.present && !fake.firewall_active);
	mesh_services_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	unlinkat(root, "tmp/dnsmasq.cfg1.d/foreign.conf", 0);
	CHECK("fresh portal failure snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	fake.fail_portal_restart = 1;
	CHECK("failure after atomic server snippet publication compensates before mesh", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.services.peer.present && !fake.firewall_active && fstatat(root, include, &metadata, 0) != 0);
	mesh_services_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("firewall-failure snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	fake.fail_firewall = 1;
	CHECK("fw4 reload failure cannot masquerade as portal completion", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) != MESH_RUNTIME_OK && !fake.services.peer.present);
	mesh_services_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("crash snapshot opens with finite mode recovery state", mesh_services_open(&peer, root, &config) == 0);
	CHECK("native MRP starts for crash fixture", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	CHECK("crashed MRP reacquires existing exclusive ownership", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("public committed configuration reconstructs IPv4 profile", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK && mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) == 0);
	CHECK("crash loads public pool metadata not credential payloads", mesh_services_open(&peer, root, &config) == 0 && peer.services->recovering && peer.services->disk.dns_state == 2);
	CHECK("crash validates existing radio ownership", mesh_peer_preflight(&peer, &config) == 0);
	CHECK("same-profile MRP crash recovery withdraws services in safe order", mesh_peer_recover_managed(&peer, root, &config, &plan) == MESH_RUNTIME_OK && !fake.services.peer.present && !fake.firewall_active);
	mesh_services_close(&peer);
	CHECK("interrupted DNS intent snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	CHECK("portal starts before interrupted suppression intent", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	config.hold_mesh = true;
	CHECK("mesh is held before crash in portal DHCP withdrawal", mesh_runtime_payload(&config, &plan, &held_payload) == MESH_OK && mesh_runtime_withdraw(root, dir, &config, &plan, &held_payload, &peer.ops, &transaction) == MESH_RUNTIME_OK);
	peer.services->disk.dns_state = 3;
	CHECK("suppression intent persists before replacing the server snippet", mesh_services_journal(peer.services) == 0);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_runtime_transaction_clear(&transaction);
	config.hold_mesh = false;
	CHECK("interrupted withdrawal process reacquires owner", mesh_peer_open(&peer, dir, &io) == 0);
	CHECK("interrupted suppression public inputs reconstruct", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK && mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) == 0);
	config.mrp.profile = MESH_MRP_UNSPECIFIED;
	CHECK("wrong-profile restart cannot silently reinterpret portal recovery metadata", mesh_services_open(&peer, root, &config) != 0);
	config.mrp.profile = MESH_MRP_IPV4_ONLY;
	CHECK("recognized suppression intent loads for exact-profile cleanup", mesh_services_open(&peer, root, &config) == 0 && peer.services->disk.dns_state == 3 && mesh_peer_preflight(&peer, &config) == 0);
	CHECK("crash during portal suppression finishes native cleanup without DHCP leak", mesh_peer_recover_managed(&peer, root, &config, &plan) == MESH_RUNTIME_OK && !fake.services.peer.present && !fake.firewall_active);
	mesh_services_close(&peer);
	CHECK("changed-source portal snapshot opens", mesh_services_open(&peer, root, &config) == 0);
	CHECK("changed-source portal starts", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_OK);
	append(root, "etc/config/firewall", "# PUBLIC-TEST-ONLY external source update\n");
	CHECK("changed committed firewall prevents new apply", mesh_peer_managed_apply(&peer, root, &config, &plan, NULL, &transaction) == MESH_RUNTIME_CONFLICT);
	CHECK("changed committed firewall still permits owned withdrawal", mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) == MESH_RUNTIME_OK && !fake.services.peer.present);
	CHECK("native Linux UDP table decoder proves selected process DHCP socket inode", mesh_mrp_udp67_entry("195: 00000000:0043 00000000:0000 07 00000000:00000000 00:00000000 00000000 453 0 12345 2 0000000000000000 0", &socket_inode) == 0 && socket_inode == 12345);
	CHECK("another UDP port is not accepted as DHCP server readiness", mesh_mrp_udp67_entry("195: 00000000:0035 00000000:0000 07 00000000:00000000 00:00000000 00000000 453 0 12345 2 0000000000000000 0", &socket_inode) != 0);
	CHECK("truncated kernel socket evidence cannot claim DHCP listener ownership", mesh_mrp_udp67_entry("195: 00000000:0043", &socket_inode) != 0);
	CHECK("native NFT argv rejects injected chain names before spawning", mesh_mrp_live_chain("forward_lan;INVALID", &held_payload) != 0);
	dns_packets();
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_native_config_clear(&config);
	mesh_runtime_transaction_clear(&transaction);
	mesh_runtime_data_clear(&payload);
	mesh_runtime_data_clear(&held_payload);
	close(dir);
	close(root);
	printf("%u MRP checks; %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
