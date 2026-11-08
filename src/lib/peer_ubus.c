/**
 * @file peer_ubus.c
 * @brief Call libubus with bounded replies and a three-second timeout.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _POSIX_C_SOURCE 200809L
#include "peer.h"
#include "mrp.h"
#include <errno.h>
#include <fcntl.h>
#include <libubus.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

struct mesh_live_bus {
	struct ubus_context *ubus;
	bool permit_mutation;
};

struct reply_copy {
	struct mesh_runtime_data *data;
	bool invalid;
	bool received;
};

static void receive(struct ubus_request *request, int type, struct blob_attr *message)
{
	struct reply_copy *copy = request->priv;
	size_t length;
	(void)type;
	if (message == NULL) {
		copy->invalid = true;
		return;
	}
	length = blob_pad_len(message);
	if (copy->received || length > sizeof(copy->data->bytes)) {
		copy->invalid = true;
		return;
	}
	memcpy(copy->data->bytes, message, length);
	copy->data->length = length;
	copy->received = true;
	if (!mesh_reply_valid(copy->data)) {
		copy->invalid = true;
	}
}

static int invoke(void *context, const char *object, const char *method,
				  struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct mesh_live_bus *bus = context;
	struct reply_copy copy = { .data = reply };
	uint32_t id;
	int result;
	bool readonly;

	readonly = strcmp(method, "status") == 0 || strcmp(method, "list") == 0 ||
			   strcmp(method, "iface_status") == 0 || strcmp(method, "ipv4leases") == 0 ||
			   strcmp(method, "ipv6leases") == 0 || strcmp(method, "ipv6ra") == 0;
	if (!readonly && !bus->permit_mutation) {
		return UBUS_STATUS_PERMISSION_DENIED;
	}
	mesh_runtime_data_clear(reply);
	result = ubus_lookup_id(bus->ubus, object, &id);
	if (result != 0) {
		return result;
	}
	result = ubus_invoke(bus->ubus, id, method, request, receive, &copy, 3000);
	if (copy.invalid) {
		mesh_runtime_data_clear(reply);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	return result;
}

static uint64_t now_ms(void *context)
{
	struct timespec now;
	(void)context;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void wait_ms(void *context, unsigned int milliseconds)
{
	struct timespec interval;
	(void)context;
	interval.tv_sec = milliseconds / 1000;
	interval.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
	}
}

static enum mesh_error interfaces(void *context, struct mesh_interface_list *output)
{
	(void)context;
	return mesh_probe_interfaces(output);
}

static enum mesh_error wiphy(void *context, uint32_t index, uint32_t frequency, struct mesh_radio *radio)
{
	(void)context;
	return mesh_probe_radio(index, frequency, radio);
}

static int verify_bridge(void *context, const struct mesh_plan *plan)
{
	uint8_t mac[6];
	char path[128];
	char target[512];
	char stp[16];
	char *base;
	size_t size;
	ssize_t length;
	int root;
	int result = -1;
	(void)context;

	if (mesh_probe_bridge(plan->bridge_ifname, mac) != MESH_OK ||
		memcmp(mac, plan->identity.bridge_mac, sizeof(mac)) != 0) {
		return -1;
	}
	root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return -1;
	}
	snprintf(path, sizeof(path), "sys/class/net/%s/bridge/stp_state", plan->bridge_ifname);
	if (mesh_read_bounded_at(root, path, stp, sizeof(stp), &size, NULL) != 0 ||
		!((size == 2 && memcmp(stp, "0\n", 2) == 0) || (size == 1 && stp[0] == '0'))) {
		goto done;
	}
	snprintf(path, sizeof(path), "sys/class/net/%s/master", plan->mesh_ifname);
	length = mesh_readlink_at_root(root, path, target, sizeof(target) - 1);
	if (length <= 0 || (size_t)length >= sizeof(target) - 1) {
		goto done;
	}
	target[length] = '\0';
	base = strrchr(target, '/');
	if (base == NULL || strcmp(base + 1, plan->bridge_ifname) != 0 ||
		mesh_probe_ipv6_ready(plan->bridge_ifname, plan->identity.management_address) != 0) {
		goto done;
	}
	if (plan->role == MESH_ROLE_MRP) {
		char portal[64];
		snprintf(portal, sizeof(portal), "%s/64", plan->identity.portal_address);
		if (mesh_probe_ipv6_ready(plan->bridge_ifname, portal) != 0) {
			goto done;
		}
	}
	result = 0;
done:
	close(root);
	return result;
}

struct mesh_live_bus *mesh_live_bus_open(bool permit_mutation)
{
	struct mesh_live_bus *bus = calloc(1, sizeof(*bus));
	if (bus == NULL) {
		return NULL;
	}
	bus->ubus = ubus_connect(NULL);
	if (bus->ubus == NULL) {
		free(bus);
		return NULL;
	}
	bus->permit_mutation = permit_mutation;
	return bus;
}

void mesh_live_bus_close(struct mesh_live_bus *bus)
{
	if (bus != NULL) {
		ubus_free(bus->ubus);
		mesh_sensitive_clear(bus, sizeof(*bus));
		free(bus);
	}
}

static int restart_dnsmasq(void *context, const char *instance)
{
	struct mesh_live_bus *bus = context;
	char *const environment[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/", "LC_ALL=C", NULL };
	char *const arguments[] = { "/etc/init.d/dnsmasq", "restart", (char *)instance, NULL };
	size_t i;
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	pid_t child;
	pid_t waited;
	uint64_t deadline;
	unsigned int attempts = 0;
	int status;
	int spawned;
	if (!bus->permit_mutation || instance == NULL || instance[0] == '\0' || strlen(instance) >= 64) {
		return -1;
	}
	for (i = 0; instance[i] != '\0'; i++) {
		if (!((instance[i] >= 'a' && instance[i] <= 'z') || (instance[i] >= 'A' && instance[i] <= 'Z') ||
			  (instance[i] >= '0' && instance[i] <= '9') || instance[i] == '_')) {
			return -1;
		}
	}
	if (posix_spawn_file_actions_init(&actions) != 0) {
		return -1;
	}
	if (posix_spawnattr_init(&attributes) != 0) {
		posix_spawn_file_actions_destroy(&actions);
		return -1;
	}
	spawned = -1;
	if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
		posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0) {
		spawned = posix_spawn(&child, arguments[0], &actions, &attributes, arguments, environment);
	}
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0) {
		return -1;
	}
	deadline = now_ms(context) + 30000;
	while (attempts++ < 600 && now_ms(context) < deadline) {
		waited = waitpid(child, &status, WNOHANG);
		if (waited == child) {
			return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		}
		if (waited < 0 && errno != EINTR) {
			return -1;
		}
		wait_ms(context, 50);
	}
	kill(-child, SIGKILL);
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return -1;
}

static int firewall_reload(void *context)
{
	return ((struct mesh_live_bus *)context)->permit_mutation ? mesh_mrp_live_reload() : -1;
}

static int firewall_chain(void *context, const char *chain, struct mesh_runtime_data *reply)
{
	(void)context;
	return mesh_mrp_live_chain(chain, reply);
}

static int portal_probe(void *context, const char *bridge, const char *uplink,
						 uint32_t gateway, uint8_t prefix, uint32_t dns_pid)
{
	if (!((struct mesh_live_bus *)context)->permit_mutation) {
		return -1;
	}
	return mesh_mrp_live_probe(bridge, uplink, gateway, prefix, dns_pid);
}

void mesh_live_transport(struct mesh_live_bus *bus, struct mesh_peer_transport *transport)
{
	memset(transport, 0, sizeof(*transport));
	transport->context = bus;
	transport->invoke = invoke;
	transport->now_ms = now_ms;
	transport->wait_ms = wait_ms;
	transport->interfaces = interfaces;
	transport->wiphy = wiphy;
	transport->verify_bridge = verify_bridge;
	transport->restart_dnsmasq = restart_dnsmasq;
	transport->reload_firewall = firewall_reload;
	transport->firewall_chain = firewall_chain;
	transport->portal_probe = portal_probe;
}
