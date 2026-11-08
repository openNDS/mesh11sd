/**
 * @file peer_main.c
 * @brief Foreground OpenWrt peer, auto-IPv4, and IPv4 MRP entry points.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _POSIX_C_SOURCE 200809L
#include "peer.h"
#include "services.h"
#include "bridge.h"
#include "mrp.h"
#include "portal_runtime.h"
#include "wan.h"
#include "status.h"
#include "wpad_guard.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reload_requested;

static void signal_stop(int number)
{
	reload_requested = number == SIGHUP;
	stopping = 1;
}

static enum mesh_error startup_role(void *context, const char *wan, enum mesh_role *out)
{
	struct mesh_peer *peer = context;
	(void)wan;
	if (peer == NULL || out == NULL || !peer->startup_role_confirmed) {
		return MESH_E_INPUT;
	}
	*out = peer->confirmed_startup_role;
	return MESH_OK;
}

static bool safe_no_mesh(struct mesh_peer *peer, const char *ifname)
{
	struct mesh_runtime_data current = {0};
	struct mesh_interface_list interfaces;
	size_t i;
	bool safe = peer->ops.read(peer, &current) == 0 && current.length == 0 &&
				peer->transport.interfaces(peer->transport.context, &interfaces) == MESH_OK;
	for (i = 0; safe && i < interfaces.count; i++) {
		safe = strcmp(interfaces.entries[i].ifname, ifname) != 0;
	}
	mesh_runtime_data_clear(&current);
	return safe;
}

static uint64_t mesh_wpad_monotonic_ms(void *context)
{
	struct timespec now;
	(void)context;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int mesh_wpad_sleep_ms(void *context, unsigned int milliseconds)
{
	struct timespec remaining;
	(void)context;
	remaining.tv_sec = milliseconds / 1000u;
	remaining.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
	while (nanosleep(&remaining, &remaining) != 0) {
		if (errno != EINTR) {
			return -1;
		}
		return 0;
	}
	return 0;
}

static void mesh_wpad_log_stderr(void *context, const char *line)
{
	(void)context;
	fputs(line, stderr);
	fputc('\n', stderr);
}

static int run_native(int argc, char **argv)
{
	struct rlimit no_core = { 0, 0 };
	struct mesh_live_bus *bus = NULL;
	struct mesh_peer_transport transport;
	struct mesh_peer peer;
	struct mesh_native_config config;
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_runtime_data wanted;
	struct mesh_runtime_data observed;
	struct mesh_runtime_transaction transaction;
	struct mesh_discovery_ops discovery;
	struct sigaction action;
	enum mesh_runtime_result status;
	char release[4096];
	size_t length;
	size_t i;
	int root = -1;
	int directory = -1;
	int result = 1;
	bool managed = true;
	bool ipv4_mrp = argc == 2 && strcmp(argv[1], "--run-openwrt-ipv4-mrp") == 0;
	bool automatic = argc == 2 && strcmp(argv[1], "--run-openwrt-auto-ipv4") == 0;
	struct mesh_wan_result wan_health = {0};
	struct mesh_wan_io wan_io = { .probe = mesh_wan_native_probe };
	bool bridge_opened = false;
	bool role_prepared = false;
	struct mesh_bridge bridge_state = {0};
	struct mesh_radio initial_radio;
	char initial_mesh_ifname[16] = {0};
	struct mesh_portal_runtime portals;
	struct mesh_portal_advert local_advert;
	struct mesh_portal_io portal_io;
	uint8_t local_portal[16];
	char portal_status[512];
	uint32_t bridge_index = 0;
	uint32_t mesh_index = 0;
	uint64_t next_portal_ms = 0;
	uint64_t now_ms;
	int portal_socket = -1;
	int portal_result;
	uint64_t next_verify_ms;
	bool portal_opened = false;
	bool portal_cleanup_conflict = false;
	bool announced = false;
	struct mesh_public_status public_status = { .role = MESH_ROLE_PEER, .automatic_at_boot = automatic };
	struct mesh_status_service *status_service = NULL;

	if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		puts("mesh11sd v8 experimental C scratch milestone8; not a validated release");
		return 0;
	}
	if ((argc == 2 && strcmp(argv[1], "--status") == 0) ||
		(argc == 3 && strcmp(argv[1], "status") == 0 && strcmp(argv[2], "--json") == 0)) {
		if (mesh_status_query(NULL,&public_status) != 0) {
			fputs("owned mesh11sd.v8 status unavailable\n",stderr);
			return 1;
		}
		return mesh_status_print(&public_status) == 0 ? 0 : 1;
	}
	if (argc != 2 || (strcmp(argv[1], "--run-openwrt-peer") != 0 && !ipv4_mrp && !automatic)) {
		fputs("usage: mesh11sd-peer --version | --status | status --json | --run-openwrt-auto-ipv4 | --run-openwrt-peer | --run-openwrt-ipv4-mrp (explicit IPv4-only beta opt-in, OpenWrt only)\n", stderr);
		return 2;
	}
	memset(&peer, 0, sizeof(peer));
	peer.session_lock = -1;
	memset(&config, 0, sizeof(config));
	memset(&wanted, 0, sizeof(wanted));
	memset(&observed, 0, sizeof(observed));
	memset(&transaction, 0, sizeof(transaction));
	root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (geteuid() != 0 || root < 0 ||
		mesh_read_bounded_at(root, "etc/openwrt_release", release, sizeof(release) - 1, &length, NULL) != 0) {
		fputs("peer backend requires an explicitly enabled OpenWrt root\n", stderr);
		goto done;
	}
	release[length] = '\0';
	if (strstr(release, "DISTRIB_ID='OpenWrt'") == NULL) {
		goto done;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
		sigaction(SIGHUP, &action, NULL) != 0) {
		goto done;
	}
	/* Full wpad (802.11s and OWE) is required before any bus, journal, or
	 * network operation, including crash recovery. --version and status return
	 * above this point. */
	{
		struct mesh_wpad_time wpad_time;
		memset(&wpad_time, 0, sizeof(wpad_time));
		wpad_time.now_ms = mesh_wpad_monotonic_ms;
		wpad_time.sleep_ms = mesh_wpad_sleep_ms;
		wpad_time.stop = &stopping;
		wpad_time.reload = &reload_requested;
		wpad_time.log_line = mesh_wpad_log_stderr;
		if (mesh_wpad_full_wait(root, &wpad_time) != 0) {
			goto done;
		}
	}
	bus = mesh_live_bus_open(true);
	if (bus == NULL || (mkdir("/var/run/mesh11sd-v8", 0700) != 0 && errno != EEXIST)) {
		goto done;
	}
	directory = open("/var/run/mesh11sd-v8", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	mesh_live_transport(bus, &transport);
	if (directory < 0 || mesh_peer_open(&peer, directory, &transport) != 0) {
		fputs("peer ownership unavailable\n", stderr);
		goto done;
	}
	status_service = mesh_status_open(&public_status,NULL);
	if (status_service == NULL) {
		fputs("exclusive mesh11sd.v8 status ownership unavailable\n",stderr);
		goto done;
	}
	peer.cancelled = &stopping;
	memset(&discovery, 0, sizeof(discovery));
	discovery.context = &peer;
	discovery.radio_config = mesh_peer_radio;
	if (mesh_native_config_read(root, &config) != MESH_OK) {
		goto done;
	}
	if (automatic || ipv4_mrp) {
		if (mesh_wan_assess(root, &peer, &config, &wan_io, &wan_health) != 0) {
			fputs("committed WAN evidence invalid; automatic startup refused\n", stderr);
			goto done;
		}
	}
	if (mesh_wan_boot_role(wan_health.health,automatic,ipv4_mrp,&peer.confirmed_startup_role) != 0) {
		fputs("explicit MRP requires its own source-bound configured-DNS path; startup refused\n",stderr);
		goto done;
	}
	if (automatic || ipv4_mrp) {
		fprintf(stderr,"wan_health=%s internet_verified=0 startup_role=%s\n",
				mesh_wan_health_name(wan_health.health), peer.confirmed_startup_role == MESH_ROLE_MRP ? "mrp" : "peer");
	}
	if (mesh_wan_configured_role(config.portal_detect,peer.confirmed_startup_role) != 0) {
		fputs("committed portal mode conflicts with qualified startup role or is unsupported; no network enabled\n",stderr);
		goto done;
	}
	ipv4_mrp = peer.confirmed_startup_role == MESH_ROLE_MRP;
	public_status.role = ipv4_mrp ? MESH_ROLE_MRP : MESH_ROLE_PEER;
	public_status.wan_assessed = automatic || ipv4_mrp;
	public_status.wan_health = wan_health.health;
	public_status.phase = MESH_STATUS_STARTING;
	peer.startup_role_confirmed = true;
	discovery.role = startup_role;
	if (ipv4_mrp && mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0) {
		fputs("IPv4-only portal WAN/firewall/DNS-owner evidence unavailable; no network enabled\n", stderr);
		goto done;
	}
	memcpy(initial_mesh_ifname, config.mesh_ifname, sizeof(initial_mesh_ifname));
	/* Portal renewal always targets our private DHCP alias. Even an already
	 * prepared LAN goes through the existing backed-up controlled transition;
	 * do not silently reuse/signal its potentially foreign DHCP client. */
	if (managed && mesh_services_open(&peer, root, &config) != 0) {
		fputs("role-service ownership/evidence unavailable; no mesh enabled\n", stderr);
		goto done;
	}
	status = mesh_peer_recover_detached(&peer, root, &config);
	if (status != MESH_RUNTIME_OK && status != MESH_RUNTIME_UNCHANGED) {
		fputs("interrupted peer withdrawal requires recovery; foreign resources preserved\n", stderr);
		goto done;
	}
	if (mesh_peer_preflight(&peer, &config) != 0 || mesh_peer_radio(&peer, &config, &initial_radio) != MESH_OK) {
		goto done;
	}
	if (managed && peer.services->recovering && !peer.present) {
		if (!safe_no_mesh(&peer, initial_mesh_ifname) || mesh_services_restore(&peer) != 0) {
			goto done;
		}
		mesh_services_close(&peer);
		if (mesh_services_open(&peer, root, &config) != 0) {
			goto done;
		}
	}
	if (mesh_bridge_open(&bridge_state, &peer, root, &config) != 0) {
		fputs("LAN bridge mapping/ownership evidence unavailable; no mesh enabled\n", stderr);
		goto done;
	}
	bridge_opened = true;
	if (bridge_state.disk.phase == 3) {
		if (!safe_no_mesh(&peer, initial_mesh_ifname) || mesh_bridge_stop(&bridge_state) != 0 ||
			mesh_bridge_open(&bridge_state, &peer, root, &config) != 0) {
			goto done;
		}
	}
	if (managed && !peer.services->recovering) {
		if (mesh_services_prepare(&peer) != 0) {
			goto done;
		}
		role_prepared = !peer.present;
	}
	if (mesh_bridge_apply(&bridge_state) != 0) {
		fputs("owned bridge creation/verification failed; ordered cleanup required\n", stderr);
		goto done;
	}
	if (mesh_native_plan(root, &discovery, &config, &facts, &plan) != MESH_OK) {
		goto done;
	}
	config.manage_services = managed;
	if (ipv4_mrp && (plan.role != MESH_ROLE_MRP || mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0)) {
		goto done;
	}
	if (mesh_peer_preflight(&peer, &config) != 0 || mesh_runtime_payload(&config, &plan, &wanted) != MESH_OK) {
		fputs("peer prerequisites or role not ready\n", stderr);
		goto done;
	}
	status = role_prepared ? MESH_RUNTIME_UNCHANGED :
			 managed ? mesh_peer_recover_managed(&peer, root, &config, &plan) :
					   mesh_peer_recover(&peer, root, &config, &plan, &wanted);
	if (status != MESH_RUNTIME_OK && status != MESH_RUNTIME_UNCHANGED) {
		fputs("peer recovery conflict; existing resources preserved\n", stderr);
		goto done;
	}
	if (managed && !role_prepared) {
		mesh_services_close(&peer);
		if (mesh_services_open(&peer, root, &config) != 0) {
			goto done;
		}
	}
	if (mesh_native_plan(root, &discovery, &config, &facts, &plan) != MESH_OK) {
		goto done;
	}
	config.manage_services = managed;
	if (ipv4_mrp && (plan.role != MESH_ROLE_MRP || mesh_mrp_configure(root, &peer, &config, MESH_MRP_IPV4_ONLY) != 0)) {
		goto done;
	}
	if (mesh_peer_preflight(&peer, &config) != 0 || mesh_runtime_payload(&config, &plan, &wanted) != MESH_OK) {
		goto done;
	}
	peer.config = &config;
	peer.plan = &plan;
	status = managed ? mesh_peer_managed_apply(&peer, root, &config, &plan, &stopping, &transaction) :
					   mesh_runtime_apply(root, directory, &config, &plan, &wanted, &peer.ops, &stopping, false, &transaction);
	if (status != MESH_RUNTIME_OK) {
		fputs("peer apply failed; inspect ownership/recovery status\n", stderr);
		if (transaction.active) {
			if (managed) {
				mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction);
			} else {
				mesh_runtime_stop(directory, &peer.ops, &transaction);
			}
		}
		goto done;
	}
	bridge_index = if_nametoindex(config.bridge_ifname);
	mesh_index = if_nametoindex(config.mesh_ifname);
	memset(&portal_io, 0, sizeof(portal_io));
	portal_io.context = &peer;
	portal_io.measure = mesh_portal_live_measure;
	portal_io.probe = mesh_portal_live_probe;
	portal_io.neighbor_get = mesh_portal_native_get;
	portal_io.neighbor_set = mesh_portal_native_set;
	portal_io.client_ready = mesh_portal_live_client_ready;
	portal_io.renew = mesh_portal_live_renew;
	if (inet_pton(AF_INET6, plan.identity.portal_address, local_portal) != 1 ||
		mesh_portal_local_advert(&peer, &local_advert) != 0 ||
		mesh_portal_runtime_open(&portals, &peer, &portal_io, bridge_index, mesh_index,
								 config.config.portal_ipv4.address) != 0) {
		fputs("portal identity/path/owned ARP recovery unavailable; ordered withdrawal required\n", stderr);
		goto done;
	}
	portal_opened = true;
	portal_socket = mesh_portal_lldp_open(mesh_index);
	if (portal_socket < 0) {
		goto done;
	}
	fputs("local netifd/wpad/kernel configuration verified; native portal observation active; hardware mesh not validated\n", stderr);
	public_status.phase = MESH_STATUS_ACTIVE;
	public_status.mesh_active = true;
	public_status.ipv4_services_active = ipv4_mrp;
	result = 0;
	while (!stopping) {
		next_verify_ms = transport.now_ms(transport.context) + 1000;
		do {
			uint64_t current_ms = transport.now_ms(transport.context);
			unsigned int wait_ms = current_ms >= next_verify_ms ? 0 : (unsigned int)(next_verify_ms-current_ms);
			if (mesh_status_dispatch(status_service,wait_ms) != 0) {
				result = 1;
				break;
			}
		} while (!stopping && transport.now_ms(transport.context) < next_verify_ms);
		if (result != 0 || stopping) {
			break;
		}
		if (peer.ops.read(&peer, &observed) != 0 || observed.length != wanted.length ||
			memcmp(observed.bytes, wanted.bytes, wanted.length) != 0 || mesh_services_verify(&peer) != 0 ||
			mesh_bridge_verify(&bridge_state) != 0 || (ipv4_mrp && mesh_mrp_verify(&peer) != 0)) {
			result = 1;
			break;
		}
		now_ms = transport.now_ms(transport.context);
		if (mesh_portal_lldp_receive(portal_socket, mesh_index, &portals, local_portal, now_ms / 1000) != 0) {
			result = 1;
			break;
		}
		if (now_ms >= next_portal_ms) {
			if (ipv4_mrp && (mesh_wan_assess(root, &peer, &config, &wan_io, &wan_health) != 0 ||
				mesh_wan_role_guard(peer.confirmed_startup_role,wan_health.health) != 0)) {
				public_status.wan_health = wan_health.health;
				fputs("own WAN qualification lost; withdraw owned MRP before restart/requalification\n", stderr);
				result = 1;
				break;
			}
			if (mesh_portal_local_advert(&peer, &local_advert) != 0 ||
				(ipv4_mrp && mesh_portal_lldp_send(portal_socket, mesh_index, &local_advert) != 0)) {
				result = 1;
				break;
			}
			public_status.wan_health = wan_health.health;
			announced = announced || ipv4_mrp;
			portal_result = mesh_portal_round(&portals, &plan, now_ms);
			if (portal_result < 0 || mesh_portal_runtime_status(&portals, portal_status, sizeof(portal_status)) < 0) {
				result = 1;
				break;
			}
			/* Only fixed public diagnostics and public next-hop MACs; neither
			 * the on-air ID nor credential or confidential UCI data is logged. */
			public_status.portal_selected = portals.selection.selected;
			fputs(portal_status, stderr);
			next_portal_ms = transport.now_ms(transport.context) + MESH_PORTAL_HELLO_SECONDS * 1000;
		}
		for (i = 0; i < config.stamp_count; i++) {
			if (mesh_stamp_unchanged(root, &config.stamps[i]) != 0) {
				stopping = 1;
				break;
			}
		}
	}
done:
	public_status.phase = MESH_STATUS_WITHDRAWING;
	if (status_service != NULL) {
		(void)mesh_status_dispatch(status_service,0);
	}
	if (stopping) {
		result = 0;
	}
	if (announced && portal_socket >= 0) {
		local_advert.ttl = 0;
		if (mesh_portal_lldp_send(portal_socket, mesh_index, &local_advert) != 0) {
			result = 1;
		}
	}
	if (portal_socket >= 0) {
		close(portal_socket);
	}
	if (portal_opened && mesh_portal_runtime_stop(&portals) != 0) {
		fputs("owned ARP cleanup conflict; bridge anchor retained for explicit recovery\n", stderr);
		portal_cleanup_conflict = true;
		result = 1;
	}
	if (bridge_opened) {
		if (transaction.active) {
			status = managed ? mesh_peer_managed_stop(&peer, root, &config, &plan, &transaction) :
							   mesh_runtime_stop(directory, &peer.ops, &transaction);
			if (status != MESH_RUNTIME_OK && status != MESH_RUNTIME_UNCHANGED) {
				fputs("peer cleanup requires recovery; reload refused\n", stderr);
				result = 1;
			}
		}
		if (safe_no_mesh(&peer, initial_mesh_ifname)) {
			if (peer.services != NULL && mesh_services_restore(&peer) != 0) {
				result = 1;
			}
			if (portal_cleanup_conflict || mesh_bridge_stop(&bridge_state) != 0) {
				fputs("owned bridge cleanup requires recovery; foreign changes preserved\n", stderr);
				result = 1;
			}
		} else {
			result = 1;
		}
	}
	public_status.phase = result == 0 ? MESH_STATUS_WITHDRAWING : MESH_STATUS_CONFLICT;
	if (status_service != NULL) {
		(void)mesh_status_dispatch(status_service,0);
	}
	mesh_status_close(status_service);
	mesh_bridge_clear(&bridge_state);
	mesh_runtime_data_clear(&observed);
	mesh_runtime_data_clear(&wanted);
	mesh_runtime_transaction_clear(&transaction);
	mesh_native_config_clear(&config);
	mesh_services_close(&peer);
	mesh_peer_close(&peer);
	mesh_live_bus_close(bus);
	if (directory >= 0) {
		close(directory);
	}
	if (root >= 0) {
		close(root);
	}
	return result;
}

int main(int argc, char **argv)
{
	int result;
	do {
		stopping = 0;
		reload_requested = 0;
		result = run_native(argc, argv);
		/* Explicit HUP is stop/restore/replan/start, never an accumulating
		 * extra netifd instance. Failed cleanup MUST prevent the new apply. */
	} while (result == 0 && reload_requested);
	return result;
}
