/**
 * @file portal_runtime.c
 * @brief Run portal selection, the neighbor pin, and the renew request.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "portal_runtime.h"
#include <arpa/inet.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>

static bool same(const struct mesh_neighbor *a, const struct mesh_neighbor *b)
{
	return a->exists == b->exists && (!a->exists || (memcmp(a->mac, b->mac, 6) == 0 &&
		   a->state == b->state && a->flags == b->flags && a->type == b->type));
}
static bool identity(const uint8_t id[16], const uint8_t local[16])
{
	static const uint8_t zero[16];
	static const uint8_t suffix[8] = {0, 0, 0, 0, 0, 0, 0, 1};
	return memcmp(id, zero, 16) == 0 || (id[0] == 0xfd && memcmp(id, local, 6) == 0 && memcmp(id + 8, suffix, 8) == 0);
}
/* A restored dynamic neighbor may legitimately age between ACK and read-back.
 * This is not a promise to preserve the kernel cache's transient NUD timer. */
static bool restored(const struct mesh_neighbor *current, const struct mesh_neighbor *previous)
{
	if (same(current, previous)) {
		return true;
	}
	return current->exists && previous->exists && current->flags == previous->flags &&
		   current->type == previous->type && memcmp(current->mac, previous->mac, 6) == 0 &&
		   (current->state & (NUD_PERMANENT | NUD_NOARP)) == 0 &&
		   (previous->state & (NUD_PERMANENT | NUD_NOARP)) == 0;
}
int mesh_portal_runtime_stop(struct mesh_portal_runtime *runtime)
{
	struct mesh_neighbor current;
	if (runtime == NULL) {
		return -1;
	}
	if (runtime->phase == 0) {
		return runtime->journal_exists && mesh_stamp_unchanged(runtime->directory, &runtime->stamp) != 0 ? -1 : 0;
	}
	if (!runtime->journal_exists || mesh_stamp_unchanged(runtime->directory, &runtime->stamp) != 0 ||
		runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0) {
		return -1;
	}
	if (!current.exists) {
		/* netifd may have flushed the entire dynamic device during recovery.
		 * Missing cache state is not a foreign object to overwrite or recreate. */
		memset(&runtime->previous, 0, sizeof(runtime->previous));
	} else if (runtime->phase == 3) {
		if (!same(&current, &runtime->installed) && !restored(&current, &runtime->previous)) {
			return -1;
		}
	} else if (!same(&current, &runtime->installed) &&
			   (runtime->phase != 1 || !same(&current, &runtime->pending))) {
		return -1;
	}
	if (!restored(&current, &runtime->previous)) {
		if (runtime->phase != 3) {
			runtime->installed = current;
			memset(&runtime->pending, 0, sizeof(runtime->pending));
			runtime->phase = 3;
			if (mesh_portal_journal_store(runtime) != 0) {
				return -1;
			}
		}
		/* The restore intent must precede the kernel mutation too. Recovery
		 * accepts either our old pin or this exact previous dynamic identity. */
		if (runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0 ||
			!same(&current, &runtime->installed) ||
			runtime->io.neighbor_set(runtime->io.context, runtime->bridge_index, runtime->gateway, &runtime->previous) != 0 ||
			runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0 ||
			!restored(&current, &runtime->previous)) {
			return -1;
		}
	}
	runtime->phase = 0;
	runtime->pin_active = false;
	memset(&runtime->installed, 0, sizeof(runtime->installed));
	memset(&runtime->previous, 0, sizeof(runtime->previous));
	memset(&runtime->pending, 0, sizeof(runtime->pending));
	/* Keep only public last-identity/notification evidence until reboot. A
	 * clean restart or role-attribute change must not manufacture another renew. */
	return mesh_portal_journal_store(runtime);
}
int mesh_portal_runtime_open(struct mesh_portal_runtime *runtime, struct mesh_peer *peer,
							 const struct mesh_portal_io *io, uint32_t bridge_index,
							 uint32_t mesh_index, uint32_t gateway)
{
	uint8_t local[16];
	struct mesh_neighbor old;
	int probed;
	bool rebound = false;
	if (runtime == NULL || peer == NULL || io == NULL || peer->plan == NULL || peer->config == NULL ||
		!peer->config->manage_services ||
		peer->session_lock < 0 || peer->directory < 0 || peer->transport.now_ms == NULL || bridge_index == 0 || mesh_index == 0 ||
		bridge_index == mesh_index || gateway == 0 || io->measure == NULL || io->probe == NULL ||
		io->neighbor_get == NULL || io->neighbor_set == NULL || io->client_ready == NULL || io->renew == NULL ||
		inet_pton(AF_INET6, peer->plan->identity.portal_address, local) != 1) {
		return -1;
	}
	memset(runtime, 0, sizeof(*runtime));
	runtime->peer = peer;
	runtime->io = *io;
	runtime->directory = peer->directory;
	runtime->bridge_index = bridge_index;
	runtime->mesh_index = mesh_index;
	runtime->gateway = gateway;
	if (mesh_portal_journal_load(runtime) != 0) {
		return -1;
	}
	if (runtime->gateway != gateway) {
		if (runtime->phase != 0) {
			return -1;
		}
		runtime->gateway = gateway;
		rebound = true;
	}
	if (!identity(runtime->selection.selected_id, local) || !identity(runtime->notified, local)) {
		if (runtime->phase != 0) {
			return -1;
		}
		/* A cleanly released public history must not prevent a deliberately
		 * changed mesh group from starting. No old resource remains to adopt. */
		memset(&runtime->selection, 0, sizeof(runtime->selection));
		memset(runtime->notified, 0, sizeof(runtime->notified));
		runtime->renew_uncertain = false;
		rebound = true;
	}
	if (runtime->bridge_index != bridge_index) {
		if (runtime->phase != 0) {
			probed = runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &old);
			if (probed != 1 && (probed != 0 || old.exists)) {
				/* Never follow a recycled index onto an extant foreign entry. */
				return -1;
			}
			runtime->phase = 0;
			runtime->pin_active = false;
			memset(&runtime->previous, 0, sizeof(runtime->previous));
			memset(&runtime->installed, 0, sizeof(runtime->installed));
			memset(&runtime->pending, 0, sizeof(runtime->pending));
		}
		runtime->bridge_index = bridge_index;
		rebound = true;
	}
	if (rebound && runtime->journal_exists && mesh_portal_journal_store(runtime) != 0) {
		return -1;
	}
	return mesh_portal_runtime_stop(runtime);
}
static int pin(struct mesh_portal_runtime *runtime, const uint8_t mac[6])
{
	struct mesh_neighbor current;
	struct mesh_neighbor desired = { .exists = true, .state = NUD_PERMANENT, .type = RTN_UNICAST };
	memcpy(desired.mac, mac, 6);
	if (runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0) {
		return -1;
	}
	if (runtime->phase != 0) {
		if (runtime->phase != 2 || !same(&current, &runtime->installed)) {
			return -1;
		}
		if (same(&current, &desired)) {
			return 0;
		}
	} else {
		/* A configured static/proxy/external entry is not ours to replace. */
		if (current.exists && ((current.state & (NUD_PERMANENT | NUD_NOARP)) != 0 ||
			current.flags != 0 || current.type != RTN_UNICAST)) {
			return -1;
		}
		runtime->previous = current;
		runtime->installed = current;
	}
	runtime->pending = desired;
	runtime->phase = 1;
	runtime->pin_active = true;
	if (mesh_portal_journal_store(runtime) != 0 ||
		runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0 ||
		!same(&current, &runtime->installed) ||
		runtime->io.neighbor_set(runtime->io.context, runtime->bridge_index, runtime->gateway, &desired) != 0 ||
		runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &current) != 0 ||
		!same(&current, &desired)) {
		return -1;
	}
	runtime->installed = desired;
	memset(&runtime->pending, 0, sizeof(runtime->pending));
	runtime->phase = 2;
	return mesh_portal_journal_store(runtime);
}
static int observed_time(struct mesh_portal_runtime *runtime, uint64_t started,
						 uint64_t supplied, uint64_t *current)
{
	uint64_t clock = runtime->peer->transport.now_ms(runtime->peer->transport.context);
	if ((runtime->peer->cancelled != NULL && *runtime->peer->cancelled) ||
		clock < started || UINT64_MAX - supplied < clock - started) {
		return -1;
	}
	*current = supplied + clock - started;
	return 0;
}
int mesh_portal_round(struct mesh_portal_runtime *runtime, const struct mesh_plan *plan, uint64_t now_ms)
{
	struct mesh_portal portals[MESH_PORTALS_MAX];
	struct mesh_portal_state next;
	struct mesh_portal_result action;
	struct mesh_portal_record *selected = NULL;
	struct mesh_path_sample path;
	struct mesh_station_sample station;
	uint8_t local[16];
	size_t count = 0;
	size_t i;
	size_t position;
	uint64_t started;
	uint64_t sampled_ms;
	uint64_t now = now_ms / 1000;
	bool fresh;
	int measured;
	int ready;
	struct mesh_neighbor actual;
	if (runtime == NULL || plan == NULL || runtime->peer == NULL || runtime->peer->config == NULL ||
		runtime->peer->transport.now_ms == NULL || plan->role == MESH_ROLE_MBP ||
		inet_pton(AF_INET6, plan->identity.portal_address, local) != 1 ||
		(runtime->peer->cancelled != NULL && *runtime->peer->cancelled) ||
		(runtime->selection.observed && now <= runtime->selection.last_observation_seconds) ||
		(runtime->journal_exists && mesh_stamp_unchanged(runtime->directory, &runtime->stamp) != 0)) {
		return -1;
	}
	if (runtime->phase != 0 && (runtime->phase != 2 ||
		runtime->io.neighbor_get(runtime->io.context, runtime->bridge_index, runtime->gateway, &actual) != 0 ||
		!same(&actual, &runtime->installed))) {
		return -1;
	}
	if (plan->role == MESH_ROLE_MRP) {
		/* Concurrent routed portals remain independent; hearing another does
		 * not change this role, tear down backhaul, or renew a static gateway. */
		if (runtime->phase != 0) {
			return -1;
		}
		if (runtime->selection.selected) {
			memset(&runtime->selection, 0, sizeof(runtime->selection));
			if (runtime->journal_exists && mesh_portal_journal_store(runtime) != 0) {
				return -1;
			}
		}
		runtime->selection.observed = true;
		runtime->selection.last_observation_seconds = now;
		runtime->rounds++;
		return 0;
	}
	started = runtime->peer->transport.now_ms(runtime->peer->transport.context);
	for (i = 0; i < MESH_PORTALS_MAX; i++) {
		struct mesh_portal_record *record = &runtime->records[i];
		if (runtime->peer->cancelled != NULL && *runtime->peer->cancelled) {
			return -1;
		}
		if (!record->used || memcmp(record->advert.id, local, 16) == 0) {
			continue;
		}
		if (observed_time(runtime, started, now_ms, &sampled_ms) != 0) {
			return -1;
		}
		now = sampled_ms / 1000;
		fresh = now >= record->seen_seconds && now - record->seen_seconds < record->advert.ttl;
		portals[count] = (struct mesh_portal){ .role = record->advert.role };
		memcpy(portals[count].id, record->advert.id, 16);
		record->metric_valid = false;
		record->probe_valid = fresh && runtime->io.probe(runtime->io.context, &record->advert) == 0;
		if (record->probe_valid && runtime->io.measure(runtime->io.context, runtime->mesh_index, record->advert.station, &path, &station) == 0 &&
			path.active && path.ifindex == runtime->mesh_index && station.ifindex == runtime->mesh_index &&
			memcmp(path.destination, record->advert.station, 6) == 0 && memcmp(path.next_hop, station.mac, 6) == 0) {
			if (observed_time(runtime, started, now_ms, &sampled_ms) != 0) {
				return -1;
			}
			memcpy(record->next_hop, path.next_hop, 6);
			measured = mesh_calc_airtime_metric(&record->window, &station, path.hops,
						 runtime->peer->config->mobility.rssi_threshold < 0,
						 runtime->peer->config->mobility.rssi_threshold, sampled_ms, &record->metric);
			if (measured == 0) {
				record->metric_valid = true;
				portals[count].metric = record->metric.adjusted_metric;
			}
		}
		portals[count].reachable = record->probe_valid;
		portals[count].metric_unavailable = !record->metric_valid;
		count++;
	}
	if (observed_time(runtime, started, now_ms, &sampled_ms) != 0) {
		return -1;
	}
	now = sampled_ms / 1000;
	position = 0;
	for (i = 0; i < MESH_PORTALS_MAX; i++) {
		struct mesh_portal_record *record = &runtime->records[i];
		if (!record->used || memcmp(record->advert.id, local, 16) == 0) {
			continue;
		}
		/* Earlier probes must still have a live claim at decision time, not
		 * merely at the beginning of a potentially slow multi-portal round. */
		if (now < record->seen_seconds || now - record->seen_seconds >= record->advert.ttl) {
			record->probe_valid = false;
			record->metric_valid = false;
			portals[position].reachable = false;
			portals[position].metric_unavailable = true;
		}
		position++;
	}
	next = runtime->selection;
	if (mesh_portal_update(&next, portals, count, plan->role, local, now, &action) != MESH_OK) {
		return -1;
	}
	if (next.selected) {
		for (i = 0; i < MESH_PORTALS_MAX; i++) {
			if (runtime->records[i].used && memcmp(runtime->records[i].advert.id, next.selected_id, 16) == 0) {
				selected = &runtime->records[i];
				break;
			}
		}
		/* During one/two failed qualified rounds retain the previous pin; do
		 * not use a stale advertisement to change its MAC. */
		if (selected != NULL && selected->metric_valid && pin(runtime, selected->advert.bridge) != 0) {
			return -1;
		}
	} else if (runtime->phase != 0 && mesh_portal_runtime_stop(runtime) != 0) {
		return -1;
	}
	runtime->selection = next;
	for (i = 0; i < MESH_PORTALS_MAX; i++) {
		struct mesh_portal_record *record = &runtime->records[i];
		if (record->used && now >= record->seen_seconds && now - record->seen_seconds >= record->advert.ttl &&
			(!next.selected || memcmp(record->advert.id, next.selected_id, 16) != 0)) {
			record->used = false;
		}
	}
	runtime->rounds++;
	if (action.identity_changed && mesh_portal_journal_store(runtime) != 0) {
		return -1;
	}
	if (selected != NULL && selected->probe_valid && !selected->metric_valid) {
		return 1;
	}
	if (!next.selected || runtime->phase != 2 || memcmp(runtime->notified, next.selected_id, 16) == 0) {
		return runtime->renew_uncertain ? 2 : 0;
	}
	ready = runtime->io.client_ready(runtime->io.context);
	if (runtime->peer->cancelled != NULL && *runtime->peer->cancelled) {
		return -1;
	}
	if (ready != 0) {
		return ready > 0 ? 1 : -1;
	}
	/* netifd has no request-ID/CAS API. Journal intent before sending once;
	 * an interrupted/uncertain request is reported, NOT resent every poll. */
	memcpy(runtime->notified, next.selected_id, 16);
	runtime->renew_uncertain = true;
	if (mesh_portal_journal_store(runtime) != 0) {
		return -1;
	}
	if ((runtime->peer->cancelled != NULL && *runtime->peer->cancelled) ||
		runtime->io.renew(runtime->io.context) != 0) {
		return 2;
	}
	runtime->renew_uncertain = false;
	return mesh_portal_journal_store(runtime);
}
int mesh_portal_runtime_status(const struct mesh_portal_runtime *runtime, char *text, size_t capacity)
{
	unsigned int records = 0;
	unsigned int metrics = 0;
	size_t i;
	int size;
	struct mesh_airtime_result selected_metric = {0};
	uint8_t next_hop[6] = {0};
	bool selected_valid = false;
	if (runtime == NULL || text == NULL) {
		return -1;
	}
	for (i = 0; i < MESH_PORTALS_MAX; i++) {
		records += runtime->records[i].used ? 1 : 0;
		metrics += runtime->records[i].metric_valid ? 1 : 0;
		if (runtime->selection.selected && runtime->records[i].metric_valid &&
			memcmp(runtime->records[i].advert.id, runtime->selection.selected_id, 16) == 0) {
			selected_valid = true;
			selected_metric = runtime->records[i].metric;
			memcpy(next_hop, runtime->records[i].next_hop, 6);
		}
	}
	size = snprintf(text, capacity, "portal_records=%u metric_available=%u selected=%u pin_owned=%u failed_rounds=%u renew=%s last_metric=%llu rounds=%llu selected_metric_available=%u raw=%llu adjusted=%llu hops=%u tx_mbps=%u retries=%u next_hop=%02x:%02x:%02x:%02x:%02x:%02x loop_active=0\n",
					records, metrics, runtime->selection.selected ? 1U : 0U, runtime->phase == 2 ? 1U : 0U,
					runtime->selection.failed_probes, runtime->renew_uncertain ? "uncertain" :
					runtime->selection.selected && memcmp(runtime->notified, runtime->selection.selected_id, 16) != 0 ? "deferred" : "none",
					(unsigned long long)runtime->selection.metric, (unsigned long long)runtime->rounds, selected_valid ? 1U : 0U,
					(unsigned long long)selected_metric.next_hop_metric,
					(unsigned long long)selected_metric.adjusted_metric, selected_metric.hops,
					selected_metric.chosen_mbps, selected_metric.retry_delta,
					next_hop[0], next_hop[1], next_hop[2], next_hop[3], next_hop[4], next_hop[5]);
	return size < 0 || (size_t)size >= capacity ? -1 : size;
}
