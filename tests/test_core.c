/**
 * @file test_core.c
 * @brief Host fixtures for factory identity, the planner, and netlink decoders.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mesh_config.h"
#include "platform.h"
#include "fixture.h"
#include "factory.h"

#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(name, expression) do { \
	checks++; \
	if (!(expression)) { \
		printf("not ok %u - %s\n", checks, name); \
		failures++; \
	} else { \
		printf("ok %u - %s\n", checks, name); \
	} \
} while (0)

static void evidence(struct mesh_mac_evidence *value, const char *mac)
{
	value->present = true;
	(void)mesh_mac_parse(mac, value->mac);
}

static void test_factory(void)
{
	struct mesh_factory_input input;
	struct mesh_config config;
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_factory_result result;
	struct mesh_factory_result saved;
	struct mesh_factory_interface *interface;
	uint8_t expected[6];

	/* Same ordering/permanence cases as apmond's shared factory tests, with
	 * synthetic MAC substitutions rather than live test-node addresses. */
	memset(&input, 0, sizeof(input));
	memset(&result, 0xa5, sizeof(result));
	saved = result;
	CHECK("factory missing evidence refuses identity", mesh11sd_factory_mac(&input, &result) == MESH_E_IDENTITY);
	CHECK("factory failure leaves output unchanged", memcmp(&saved, &result, sizeof(saved)) == 0);
	input.interface_count = 1;
	interface = &input.interfaces[0];
	memcpy(interface->name, "br-lan", 7);
	interface->ifindex = 9;
	interface->arphrd_type = 1;
	interface->is_bridge = true;
	evidence(&interface->sysfs_address, "00:11:22:33:44:55");
	CHECK("factory never substitutes bridge identity", mesh11sd_factory_mac(&input, &result) == MESH_E_IDENTITY);
	memcpy(interface->name, "end5", 5);
	interface->is_bridge = false;
	interface->ifindex = 5;
	interface->addr_assign_type = 3;
	CHECK("factory rejects userspace-rewritten current address", mesh11sd_factory_mac(&input, &result) == MESH_E_IDENTITY);
	evidence(&interface->ethtool_permanent, "00:11:22:33:44:05");
	CHECK("factory ethtool permanent survives rewritten current MAC", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_LOWEST_IFINDEX);
	input.interface_count = 2;
	input.interfaces[1] = *interface;
	memcpy(input.interfaces[1].name, "end4", 5);
	input.interfaces[1].ifindex = 4;
	evidence(&input.interfaces[1].ethtool_permanent, "00:11:22:33:44:04");
	CHECK("factory lowest ifindex independent of inventory order", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  strcmp(result.interface_name, "end4") == 0);
	memcpy(input.interfaces[0].name, "eth0", 5);
	CHECK("factory common name precedes lowest ifindex", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_COMMON && strcmp(result.interface_name, "eth0") == 0);
	memcpy(input.interfaces[1].name, "eth1", 5);
	CHECK("factory eth1 precedes eth0", mesh11sd_factory_mac(&input, &result) == MESH_OK && strcmp(result.interface_name, "eth1") == 0);
	evidence(&input.interfaces[1].ethtool_permanent, "02:11:22:33:44:04");
	CHECK("factory common fallback refuses locally administered permanent MAC", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  strcmp(result.interface_name, "eth0") == 0);
	input.wan_device = "eth1";
	CHECK("factory explicit WAN permanent precedes common and can be local", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_WAN && strcmp(result.interface_name, "eth1") == 0);
	input.wan_device = "../invalid";
	CHECK("factory invalid WAN name safely falls through", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_COMMON);
	input.wan_device = "eth1";
	evidence(&input.first_uci_system_label, "00:11:22:33:44:33");
	CHECK("factory first system override precedes WAN", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_SYSTEM);
	evidence(&input.board_json_system_label, "00:11:22:33:44:22");
	CHECK("factory board label precedes system override", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_BOARD);
	evidence(&input.device_tree[2].local_mac_address, "00:11:22:33:44:13");
	CHECK("factory pre-jail copy precedes board label", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_DT_COPY);
	evidence(&input.device_tree[1].local_mac_address, "00:11:22:33:44:12");
	CHECK("factory proc DT precedes pre-jail copy", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_DT_PROC);
	evidence(&input.device_tree[0].local_mac_address, "00:11:22:33:44:11");
	CHECK("factory sysfs DT precedes proc DT", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  result.source == MESH_FACTORY_DT_SYSFS);
	evidence(&input.device_tree[0].mac_address, "00:11:22:33:44:10");
	(void)mesh_mac_parse("00:11:22:33:44:10", expected);
	CHECK("factory mac-address precedes local-mac-address", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  memcmp(result.mac, expected, 6) == 0);
	memset(input.device_tree[0].mac_address.mac, 0, 6);
	(void)mesh_mac_parse("00:11:22:33:44:11", expected);
	CHECK("factory zero DT MAC falls through to local-mac-address", mesh11sd_factory_mac(&input, &result) == MESH_OK &&
		  memcmp(result.mac, expected, 6) == 0);
	mesh_test_fixture(&config, &facts);
	CHECK("resolved factory evidence feeds the real configuration planner",
		  mesh11sd_plan_from_evidence(&config, &input, &facts, &plan) == MESH_OK &&
		  memcmp(plan.identity.factory_mac, expected, 6) == 0 &&
		  memcmp(plan.identity.bridge_mac, facts.bridge_mac, 6) == 0);
	input.interfaces[0].ifindex = input.interfaces[1].ifindex;
	CHECK("factory inconsistent duplicate inventory fails closed", mesh11sd_factory_mac(&input, &result) == MESH_E_INPUT);
	input.interface_count = MESH_FACTORY_INTERFACES_MAX + 1;
	CHECK("factory inventory overflow is explicit", mesh11sd_factory_mac(&input, &result) == MESH_E_INPUT);
}

static void test_identity(void)
{
	struct mesh_config c;
	struct mesh_facts f;
	struct mesh_identity a;
	struct mesh_identity b;
	uint8_t mac[6];

	mesh_test_fixture(&c, &f);
	CHECK("v7 ULA vector A computes", mesh_identity_build(c.auto_mesh_id, f.factory_mac,
														  f.bridge_mac, &a) == MESH_OK);
	CHECK("v7 shared /48 exact vector", strcmp(a.ula_prefix, "fd46:cfe5:34c5::/48") == 0);
	CHECK("v7 LAN hint vector", strcmp(a.lan_hint, "c43d") == 0);
	CHECK("v7 VXLAN hint vector", strcmp(a.vxlan_hint, "4000") == 0);
	CHECK("portal uses node subnet", strcmp(a.portal_address, "fd46:cfe5:34c5:c43d::1") == 0);
	CHECK("management IID uses actual bridge with legacy unflipped U/L bit",
		  strcmp(a.management_address, "fd46:cfe5:34c5:c43d:0211:22ff:fe33:4455/64") == 0);
	(void)mesh_mac_parse("02:00:00:00:00:0B", f.factory_mac);
	CHECK("second node computes", mesh_identity_build(c.auto_mesh_id, f.factory_mac,
													  f.bridge_mac, &b) == MESH_OK);
	CHECK("same group shares /48", strcmp(a.ula_prefix, b.ula_prefix) == 0);
	CHECK("different factory has deterministic distinct subnet", strcmp(b.lan_hint, "01d3") == 0);
	CHECK("factory change does not change bridge IID",
		  strstr(b.management_address, ":0211:22ff:fe33:4455/64") != NULL);
	CHECK("different mesh group computes", mesh_identity_build("bluewave", a.factory_mac,
															   a.bridge_mac, &b) == MESH_OK);
	CHECK("different group v7 prefix vector", strcmp(b.ula_prefix, "fd1f:1dce:b3d4::/48") == 0);
	CHECK("group change preserves factory hint", strcmp(a.lan_hint, b.lan_hint) == 0);
	CHECK("strict MAC accepts upper case", mesh_mac_parse("02:AA:00:00:00:0A", mac) == 0);
	CHECK("strict MAC rejects multicast", mesh_mac_parse("01:aa:00:00:00:0a", mac) < 0);
	CHECK("strict MAC rejects missing digits", mesh_mac_parse("2:aa:00:00:00:0a", mac) < 0);
	CHECK("strict MAC rejects trailing injection", mesh_mac_parse("02:aa:00:00:00:0a\n", mac) < 0);
	CHECK("strict MAC rejects zero", mesh_mac_parse("00:00:00:00:00:00", mac) < 0);
	CHECK("missing group fails", mesh_identity_build(NULL, f.factory_mac, f.bridge_mac, &b) == MESH_E_IDENTITY);
	memset(f.bridge_mac, 0, 6);
	CHECK("missing bridge identity fails rather than using factory",
		  mesh_identity_build(c.auto_mesh_id, f.factory_mac, f.bridge_mac, &b) == MESH_E_IDENTITY);
}

static void test_plan(void)
{
	struct mesh_config c;
	struct mesh_facts f;
	struct mesh_plan a;
	struct mesh_plan b;
	struct mesh_plan sentinel;
	char summary[512];

	mesh_test_fixture(&c, &f);
	CHECK("peer plan builds", mesh_plan_build(&c, &f, &a) == MESH_OK);
	CHECK("peer uses netifd DHCP client, no DHCP server", a.dhcp_client && !a.dhcp_server);
	CHECK("loop protection honestly inactive and no STP", !a.loop_protection_active && !a.bridge_stp);
	CHECK("repeat plan builds", mesh_plan_build(&c, &f, &b) == MESH_OK);
	CHECK("repeat byte identical", memcmp(&a, &b, sizeof(a)) == 0);
	CHECK("repeat needs no reconciliation", mesh_plan_delta(&a, &b) == 0);
	CHECK("first plan has all change categories", mesh_plan_delta(NULL, &a) == 15);
	f.detected_role = MESH_ROLE_MRP;
	CHECK("MRP plan builds", mesh_plan_build(&c, &f, &b) == MESH_OK);
	CHECK("MRP DHCP authoritative and ping with no sequential allocation",
		  b.dhcp_server && !b.dhcp_client && b.dhcp_authoritative && b.dhcp_ping &&
		  !b.dhcp_sequential && !b.dhcp_arp_check);
	CHECK("MRP preserves configured common pool and lease", b.ipv4.pool_first == c.portal_ipv4.pool_first &&
		  b.ipv4.pool_last == c.portal_ipv4.pool_last && b.ipv4.lease_seconds == 3600);
	CHECK("role change does not change management identity",
		  strcmp(a.identity.management_address, b.identity.management_address) == 0);
	CHECK("role change needs network/DHCP only", mesh_plan_delta(&a, &b) == (MESH_DELTA_NETWORK | MESH_DELTA_DHCP));
	f.detected_role = MESH_ROLE_MBP;
	CHECK("unfinished MBP network wiring fails explicitly, not fake success",
		  mesh_plan_build(&c, &f, &b) == MESH_E_ROLE_PENDING);
	f.detected_role = MESH_ROLE_MRP;
	c.portal_ipv4.pool_first = c.portal_ipv4.address;
	CHECK("DHCP pool excludes server", mesh_plan_build(&c, &f, &b) == MESH_E_POOL);
	c.portal_ipv4.pool_first = 0xc6336464U;
	CHECK("DHCP pool cannot cross subnet", mesh_plan_build(&c, &f, &b) == MESH_E_POOL);
	mesh_test_fixture(&c, &f);
	f.detected_role = MESH_ROLE_MRP;
	c.portal_ipv4.prefix = 0;
	CHECK("invalid prefix rejected without shift UB", mesh_plan_build(&c, &f, &b) == MESH_E_POOL);
	c.portal_ipv4.prefix = 24;
	c.portal_ipv4.pool_last = 0xc00002ffU;
	CHECK("broadcast cannot be leased", mesh_plan_build(&c, &f, &b) == MESH_E_POOL);
	c.portal_ipv4.pool_last = c.portal_ipv4.pool_first - 1;
	CHECK("reversed pool rejected", mesh_plan_build(&c, &f, &b) == MESH_E_POOL);
	mesh_test_fixture(&c, &f);
	c.preferred_phy = "phy0";
	c.mesh_ifname = "mesh-custom";
	c.bridge_ifname = "br-custom";
	c.ula_prefix_override = "fd12:3456:789a::/48";
	c.lan_hint_override = "BEEF";
	c.vxlan_hint_override = "CAFE";
	CHECK("configured overrides build", mesh_plan_build(&c, &f, &b) == MESH_OK);
	CHECK("configured interface names preserved", strcmp(b.mesh_ifname, "mesh-custom") == 0 &&
		  strcmp(b.bridge_ifname, "br-custom") == 0);
	CHECK("configured prefix/hints preserved canonically",
		  strcmp(b.identity.ula_prefix, "fd12:3456:789a::/48") == 0 &&
		  strcmp(b.identity.lan_hint, "beef") == 0 && strcmp(b.identity.vxlan_hint, "cafe") == 0);
	CHECK("overrides rebuild actual bridge management address",
		  strcmp(b.identity.management_address, "fd12:3456:789a:beef:0211:22ff:fe33:4455/64") == 0);
	CHECK("override repeat stable", mesh_plan_build(&c, &f, &a) == MESH_OK && mesh_plan_delta(&a, &b) == 0);
	c.ula_prefix_override = "2001:db8::/48";
	CHECK("global prefix cannot replace ULA", mesh_plan_build(&c, &f, &b) == MESH_E_ADDRESS);
	c.ula_prefix_override = "fd12:3456:789a:1::/48";
	CHECK("noncanonical /48 host bits rejected", mesh_plan_build(&c, &f, &b) == MESH_E_ADDRESS);
	c.ula_prefix_override = "fd12:3456:789a::/64";
	CHECK("wrong prefix length rejected", mesh_plan_build(&c, &f, &b) == MESH_E_ADDRESS);
	c.ula_prefix_override = NULL;
	c.lan_hint_override = "zzzz";
	CHECK("nonhex hint rejected", mesh_plan_build(&c, &f, &b) == MESH_E_ADDRESS);
	c.lan_hint_override = "cafe";
	CHECK("overlapping LAN/VXLAN subnet rejected", mesh_plan_build(&c, &f, &b) == MESH_E_ADDRESS);
	mesh_test_fixture(&c, &f);
	c.credential_available = false;
	CHECK("SAE requires platform credential", mesh_plan_build(&c, &f, &b) == MESH_E_SECURITY);
	c.security = MESH_SECURITY_EXPLICIT_OPEN;
	CHECK("explicit open network supported, never inferred", mesh_plan_build(&c, &f, &b) == MESH_OK);
	c.security = 0;
	CHECK("missing encryption policy fails closed", mesh_plan_build(&c, &f, &b) == MESH_E_SECURITY);
	mesh_test_fixture(&c, &f);
	f.radios[0].frequency_disabled = true;
	CHECK("disabled channel rejected", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	f.radios[0].frequency_disabled = false;
	f.radios[0].no_initiate_radiation = true;
	CHECK("NO_IR channel fails closed pending platform lifecycle", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	f.radios[0].no_initiate_radiation = false;
	f.radios[0].radar_required = true;
	CHECK("DFS channel fails closed pending CAC lifecycle", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	f.radios[0].radar_required = false;
	f.radios[0].mesh_supported = false;
	CHECK("nonmesh radio rejected", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	f.radios[0].mesh_supported = true;
	f.radios[0].combination_verified = false;
	CHECK("unverified interface combinations cannot claim usable radio", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	f.radios[0].combination_verified = true;
	f.radio_count = 2;
	f.radios[1] = f.radios[0];
	memcpy(f.radios[1].phy, "phy1", 5);
	CHECK("multiple eligible radios are not silently arbitrated", mesh_plan_build(&c, &f, &b) == MESH_E_AMBIGUOUS_RADIO);
	c.preferred_phy = "phy1";
	CHECK("configured radio preference honored", mesh_plan_build(&c, &f, &b) == MESH_OK && strcmp(b.phy, "phy1") == 0);
	c.preferred_phy = "phy9";
	CHECK("missing configured radio never falls back silently", mesh_plan_build(&c, &f, &b) == MESH_E_RADIO);
	mesh_test_fixture(&c, &f);
	CHECK("base plan resets", mesh_plan_build(&c, &f, &a) == MESH_OK);
	c.credential_revision++;
	CHECK("credential change token rebuilds", mesh_plan_build(&c, &f, &b) == MESH_OK);
	CHECK("credential rotation reconciles radio without storing credential",
		  mesh_plan_delta(&a, &b) == MESH_DELTA_RADIO);
	CHECK("safe summary builds", mesh_plan_summary(&b, summary, sizeof(summary)) > 0);
	CHECK("summary contains no caller IDs, credentials or address",
		  strstr(summary, c.auto_mesh_id) == NULL && strstr(summary, c.mesh_id) == NULL &&
		  strstr(summary, "credential") == NULL && strstr(summary, "fd46") == NULL);
	CHECK("summary says not applied", strstr(summary, "\"applied\":false") != NULL);
	CHECK("summary detects truncation", mesh_plan_summary(&b, summary, 8) < 0 && summary[0] == '\0');
	memset(&sentinel, 0xa5, sizeof(sentinel));
	b = sentinel;
	c.mesh_ifname = "x; touch /tmp/injected";
	CHECK("invalid name fails", mesh_plan_build(&c, &f, &b) == MESH_E_INPUT);
	CHECK("failed plan is transactional", memcmp(&sentinel, &b, sizeof(b)) == 0);
	c.mesh_ifname = NULL;
	c.mesh_id = "123456789012345678901234567890123";
	CHECK("oversized mesh ID fails instead of truncation", mesh_plan_build(&c, &f, &b) == MESH_E_INPUT);
	c.mesh_id = NULL;
	CHECK("missing on-air ID fails", mesh_plan_build(&c, &f, &b) == MESH_E_INPUT);
}

static void candidate(struct mesh_portal *portal, const char *address, uint64_t metric)
{
	memset(portal, 0, sizeof(*portal));
	(void)inet_pton(AF_INET6, address, portal->id);
	portal->metric = metric;
	portal->reachable = true;
	portal->role = MESH_ROLE_MRP;
}

static void test_portals(void)
{
	struct mesh_portal_state state;
	struct mesh_portal_state previous;
	struct mesh_portal_result action;
	struct mesh_portal p[2];
	uint8_t local[16];
	enum mesh_error error;

	memset(&state, 0, sizeof(state));
	candidate(&p[0], "fd00::1", 1000);
	candidate(&p[1], "fd00::2", 1100);
	CHECK("initial portal selection", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 0, &action) == MESH_OK);
	CHECK("best complete path selected", state.selected && memcmp(state.selected_id, p[0].id, 16) == 0);
	CHECK("real selection requests netifd renew", action.identity_changed && action.renew_netifd_client);
	p[0].role = MESH_ROLE_MBP;
	CHECK("attribute-only role change accepted", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 1, &action) == MESH_OK);
	CHECK("MRP MBP attribute change never renews", !action.identity_changed && !action.renew_netifd_client);
	p[1].metric = 799;
	CHECK("better candidate during hold-down observed", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 59, &action) == MESH_OK);
	CHECK("59 seconds cannot optimize", !action.identity_changed);
	p[1].metric = 801;
	CHECK("19.9 percent improvement observed", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 60, &action) == MESH_OK);
	CHECK("less than 20 percent cannot optimize", !action.identity_changed);
	p[1].metric = 800;
	CHECK("exact 20 percent improvement observed", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 61, &action) == MESH_OK);
	CHECK("20 percent starts a fresh challenger hold", !action.identity_changed && state.challenging &&
		  state.challenger_since_seconds == 61);
	CHECK("59 seconds of sustained improvement observed",
		  mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 120, &action) == MESH_OK && !action.identity_changed);
	CHECK("60 seconds of sustained improvement switches",
		  mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 121, &action) == MESH_OK &&
		  action.identity_changed && memcmp(state.selected_id, p[1].id, 16) == 0);
	p[1].reachable = false;
	CHECK("one failed completed probe accepted", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 122, &action) == MESH_OK);
	CHECK("one failure retains portal", !action.identity_changed && state.failed_probes == 1);
	previous = state;
	CHECK("duplicate round cannot create extra failures", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 122, &action) == MESH_E_INPUT);
	CHECK("invalid round is transactional", memcmp(&state, &previous, sizeof(state)) == 0);
	CHECK("two failed probes accepted", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 123, &action) == MESH_OK);
	CHECK("two failures retain portal", !action.identity_changed && state.failed_probes == 2);
	CHECK("third failed probe accepted", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 124, &action) == MESH_OK);
	CHECK("failure bypasses optimization hold and metric margin", action.identity_changed && action.renew_netifd_client &&
		  memcmp(state.selected_id, p[0].id, 16) == 0);
	p[0].reachable = false;
	(void)mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 125, &action);
	(void)mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 126, &action);
	error = mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 127, &action);
	CHECK("all portals lost clears selection after three rounds", error == MESH_OK && !state.selected && action.identity_changed);
	CHECK("no renew into absent portal", !action.renew_netifd_client);
	memset(&state, 0, sizeof(state));
	candidate(&p[0], "fd00::2", 1000);
	candidate(&p[1], "fd00::1", 1000);
	(void)mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 1, &action);
	CHECK("initial metric ties deterministic on binary identity", memcmp(state.selected_id, p[1].id, 16) == 0);
	p[0] = p[1];
	CHECK("duplicate identities rejected", mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 2, &action) == MESH_E_INPUT);
	p[0].metric = 0;
	CHECK("zero is a valid self/direct cost, not a missing-route sentinel",
		  mesh_portal_update(&state, p, 1, MESH_ROLE_PEER, NULL, 2, &action) == MESH_OK && state.metric == 0);
	memset(&state, 0, sizeof(state));
	candidate(&p[0], "fd00::1", UINT64_MAX);
	candidate(&p[1], "fd00::2", UINT64_MAX);
	(void)mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 0, &action);
	p[1].metric = (UINT64_MAX / 5) * 4;
	(void)mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 1, &action);
	CHECK("huge metrics compare without multiplication overflow",
		  mesh_portal_update(&state, p, 2, MESH_ROLE_PEER, NULL, 61, &action) == MESH_OK && action.identity_changed);
	memset(&state, 0, sizeof(state));
	(void)inet_pton(AF_INET6, "fd00::9", local);
	p[0].reachable = false;
	CHECK("MBP foreign report handled", mesh_portal_update(&state, p, 1, MESH_ROLE_MBP, local, 1, &action) == MESH_OK);
	CHECK("MBP conflict disables only backhaul via explicit action", action.disable_backhaul && !action.renew_netifd_client);
	CHECK("MBP conflict remains latched after report expires",
		  mesh_portal_update(&state, NULL, 0, MESH_ROLE_MBP, local, 2, &action) == MESH_OK && action.disable_backhaul);
	memset(&state, 0, sizeof(state));
	CHECK("daemon restart resets MBP latch",
		  mesh_portal_update(&state, NULL, 0, MESH_ROLE_MBP, local, 3, &action) == MESH_OK && !action.disable_backhaul);
	CHECK("candidate bound explicit", mesh_portal_update(&state, p, MESH_PORTALS_MAX + 1,
		  MESH_ROLE_PEER, NULL, 4, &action) == MESH_E_INPUT);
}

static void test_portal_resets(void)
{
	struct mesh_portal_state state;
	struct mesh_portal_result action;
	struct mesh_portal p[3];

	memset(&state, 0, sizeof(state));
	candidate(&p[0], "fd00::1", 1000);
	candidate(&p[1], "fd00::2", 1100);
	candidate(&p[2], "fd00::3", 1200);
	(void)mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 0, &action);
	p[1].metric = 700;
	(void)mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 1, &action);
	p[1].metric = 801;
	CHECK("insufficient improvement cancels sustained hold",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 60, &action) == MESH_OK &&
		  !state.challenging && !action.identity_changed);
	p[1].metric = 700;
	(void)mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 61, &action);
	p[2].metric = 600;
	CHECK("different best challenger starts a new hold",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 120, &action) == MESH_OK &&
		  !action.identity_changed && state.challenger_since_seconds == 120);
	CHECK("replacement challenger waits its own full 60 seconds",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 179, &action) == MESH_OK &&
		  !action.identity_changed);
	CHECK("replacement challenger switches after sustained hold",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 180, &action) == MESH_OK &&
		  action.identity_changed && memcmp(state.selected_id, p[2].id, 16) == 0);
	p[2].reachable = false;
	(void)mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 181, &action);
	p[2].reachable = true;
	CHECK("successful probe resets failure count",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 182, &action) == MESH_OK &&
		  state.failed_probes == 0 && !action.identity_changed);
	p[2].reachable = false;
	CHECK("failure after successful probe begins again at one",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 183, &action) == MESH_OK &&
		  state.failed_probes == 1 && !action.identity_changed);
	CHECK("clock reversal rejected",
		  mesh_portal_update(&state, p, 3, MESH_ROLE_PEER, NULL, 182, &action) == MESH_E_INPUT);
	CHECK("invalid local role rejected",
		  mesh_portal_update(&state, p, 3, (enum mesh_role)-1, NULL, 184, &action) == MESH_E_INPUT);
}

union message {
	struct nlmsghdr alignment;
	unsigned char data[2048];
};

static size_t put_attr(unsigned char *buffer, size_t offset, uint16_t type,
					   const void *data, size_t length)
{
	struct nlattr *attr;
	size_t padded;

	padded = NLA_ALIGN(NLA_HDRLEN + length);
	attr = (struct nlattr *)(buffer + offset);
	memset(attr, 0, padded);
	attr->nla_type = type;
	attr->nla_len = (uint16_t)(NLA_HDRLEN + length);
	if (length != 0) {
		memcpy(buffer + offset + NLA_HDRLEN, data, length);
	}
	return offset + padded;
}

static void test_platform(void)
{
	union message message;
	union message attrs;
	union message nested;
	union message band;
	union message freqs;
	struct mesh_radio radio;
	struct mesh_radio old;
	uint8_t mac[6];
	uint8_t expected[6];
	uint32_t frequency;
	size_t length;
	size_t offset;
	size_t nested_length;
	size_t i;
	unsigned int seed;

	memset(&message, 0, sizeof(message));
	(void)mesh_mac_parse("02:11:22:33:44:55", expected);
	length = NLMSG_ALIGN(sizeof(struct ifinfomsg));
	length = put_attr(message.data, length, IFLA_ADDRESS, expected, 6);
	nested_length = put_attr(nested.data, 0, IFLA_INFO_KIND, "bridge", 7);
	length = put_attr(message.data, length, IFLA_LINKINFO | NLA_F_NESTED, nested.data, nested_length);
	CHECK("rtnetlink actual bridge MAC decoded", mesh_decode_bridge(message.data, length, mac) == 0 && memcmp(mac, expected, 6) == 0);
	CHECK("truncated bridge attr rejected", mesh_decode_bridge(message.data, length - 1, mac) < 0);
	CHECK("missing ifinfomsg rejected", mesh_decode_bridge(message.data, 1, mac) < 0);
	CHECK("unaligned bridge fixture rejected safely", mesh_decode_bridge(message.data + 1, length - 1, mac) < 0);
	((struct nlattr *)(message.data + NLMSG_ALIGN(sizeof(struct ifinfomsg))))->nla_len = 0;
	CHECK("zero-length bridge attribute rejected", mesh_decode_bridge(message.data, length, mac) < 0);
	memset(&message, 0, sizeof(message));
	memset(&radio, 0, sizeof(radio));
	length = GENL_HDRLEN;
	length = put_attr(message.data, length, NL80211_ATTR_WIPHY_NAME, "phy3", 5);
	nested_length = put_attr(nested.data, 0, NL80211_IFTYPE_MESH_POINT, NULL, 0);
	length = put_attr(message.data, length, NL80211_ATTR_SUPPORTED_IFTYPES | NLA_F_NESTED, nested.data, nested_length);
	frequency = 2412;
	offset = put_attr(attrs.data, 0, NL80211_FREQUENCY_ATTR_FREQ, &frequency, sizeof(frequency));
	offset = put_attr(attrs.data, offset, NL80211_FREQUENCY_ATTR_NO_IR, NULL, 0);
	offset = put_attr(freqs.data, 0, 1 | NLA_F_NESTED, attrs.data, offset);
	offset = put_attr(band.data, 0, NL80211_BAND_ATTR_FREQS | NLA_F_NESTED, freqs.data, offset);
	offset = put_attr(nested.data, 0, 1 | NLA_F_NESTED, band.data, offset);
	length = put_attr(message.data, length, NL80211_ATTR_WIPHY_BANDS | NLA_F_NESTED, nested.data, offset);
	CHECK("nl80211 mesh and frequency capabilities decoded", mesh_decode_wiphy(message.data, length, 2412, &radio) == 0 &&
		  radio.mesh_supported && radio.frequency_supported && strcmp(radio.phy, "phy3") == 0);
	CHECK("nl80211 regulatory no-IR preserved", radio.no_initiate_radiation);
	CHECK("iftype bit does not fake concurrency proof", !radio.combination_verified);
	old = radio;
	CHECK("truncated nested wiphy rejected transactionally", mesh_decode_wiphy(message.data, length - 1, 2412, &radio) < 0 &&
		  memcmp(&old, &radio, sizeof(radio)) == 0);
	memset(&radio, 0, sizeof(radio));
	CHECK("other frequency does not become eligible", mesh_decode_wiphy(message.data, length, 5180, &radio) == 0 && !radio.frequency_supported);
	memset(&message, 0, sizeof(message));
	length = put_attr(message.data, GENL_HDRLEN, NL80211_ATTR_WIPHY_NAME, "phy3", 4);
	CHECK("nonterminated wiphy name rejected", mesh_decode_wiphy(message.data, length, 2412, &radio) < 0);
	length = put_attr(message.data, GENL_HDRLEN, NL80211_ATTR_WIPHY_NAME, "p\0x", 4);
	CHECK("embedded NUL wiphy name rejected", mesh_decode_wiphy(message.data, length, 2412, &radio) < 0);
	/* Bounded deterministic malformed-buffer corpus, run under ASan/UBSan too. */
	seed = 1;
	for (i = 0; i < 10000; i++) {
		size_t j;
		size_t size;

		seed = seed * 1103515245U + 12345U;
		size = seed % sizeof(message.data);
		for (j = 0; j < size; j++) {
			seed = seed * 1103515245U + 12345U;
			message.data[j] = (unsigned char)(seed >> 16);
		}
		memset(&radio, 0, sizeof(radio));
		(void)mesh_decode_wiphy(message.data, size, 2412, &radio);
		(void)mesh_decode_bridge(message.data, size, mac);
	}
	CHECK("10000 malformed native message fixtures completed", true);
}

int main(void)
{
	puts("TAP version 13");
	test_factory();
	test_identity();
	test_plan();
	test_portals();
	test_portal_resets();
	test_platform();
	printf("1..%u\n", checks);
	printf("# %u checks, %u failures\n", checks, failures);
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
