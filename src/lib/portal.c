/**
 * @file portal.c
 * @brief Apply the 20 percent, 60 second, and three-failure selection policy.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mesh_config.h"

#include <string.h>

static bool identity_valid(const uint8_t id[16])
{
	static const uint8_t zero[16];

	return id != NULL && id[0] != 0xff && memcmp(id, zero, 16) != 0;
}

static bool better(const struct mesh_portal *a, const struct mesh_portal *b)
{
	if (b == NULL || a->metric < b->metric) {
		return true;
	}
	return a->metric == b->metric && memcmp(a->id, b->id, 16) < 0;
}

static bool margin_met(uint64_t candidate, uint64_t current)
{
	/* candidate <= floor(4 * current / 5), without overflow. */
	uint64_t threshold;

	threshold = (current / 5) * 4 + ((current % 5) * 4) / 5;
	return candidate < current && candidate <= threshold;
}

enum mesh_error mesh_portal_update(struct mesh_portal_state *state,
								  const struct mesh_portal *portals, size_t count,
								  enum mesh_role local_role, const uint8_t local_id[16],
								  uint64_t now, struct mesh_portal_result *result)
{
	struct mesh_portal_state next;
	struct mesh_portal_result action;
	const struct mesh_portal *best;
	const struct mesh_portal *current;
	size_t i;
	size_t j;
	bool switch_allowed;
	bool had_selection;
	uint8_t previous[16];

	if (state == NULL || result == NULL || count > MESH_PORTALS_MAX ||
		(count != 0 && portals == NULL) || local_role < MESH_ROLE_PEER || local_role > MESH_ROLE_MBP ||
		(local_role == MESH_ROLE_MBP && !identity_valid(local_id))) {
		return MESH_E_INPUT;
	}
	if (state->observed && now <= state->last_observation_seconds) {
		return MESH_E_INPUT;
	}
	best = NULL;
	current = NULL;
	for (i = 0; i < count; i++) {
		if (!identity_valid(portals[i].id) ||
			(portals[i].role != MESH_ROLE_MRP && portals[i].role != MESH_ROLE_MBP)) {
			return MESH_E_INPUT;
		}
		for (j = 0; j < i; j++) {
			if (memcmp(portals[i].id, portals[j].id, 16) == 0) {
				return MESH_E_INPUT;
			}
		}
		if (state->selected && memcmp(portals[i].id, state->selected_id, 16) == 0) {
			current = &portals[i];
		}
		if (portals[i].reachable && !portals[i].metric_unavailable && better(&portals[i], best)) {
			best = &portals[i];
		}
	}
	next = *state;
	memset(&action, 0, sizeof(action));
	next.observed = true;
	next.last_observation_seconds = now;
	/* Hearing a foreign portal is sufficient for MBP conflict, even when its
	 * subsequent reachability probe failed. Backhaul stays off until restart. */
	if (local_role == MESH_ROLE_MBP) {
		for (i = 0; i < count; i++) {
			if (memcmp(portals[i].id, local_id, 16) != 0) {
				next.mbp_backhaul_latched_off = true;
			}
		}
	}
	if (next.mbp_backhaul_latched_off) {
		action.disable_backhaul = true;
		*state = next;
		*result = action;
		return MESH_OK;
	}
	had_selection = next.selected;
	memcpy(previous, next.selected_id, sizeof(previous));
	switch_allowed = !next.selected;
	if (next.selected) {
		if (current == NULL || !current->reachable) {
			if (next.failed_probes < MESH_FAILURE_LIMIT) {
				next.failed_probes++;
			}
			switch_allowed = next.failed_probes >= MESH_FAILURE_LIMIT;
			next.challenging = false;
		} else if (current->metric_unavailable) {
			/* A successful probe with missing/reset telemetry is NOT a failed
			 * probe. Keep the identity, invalidate the challenge, and defer
			 * comparison instead of manufacturing a cost or a renew storm. */
			next.failed_probes = 0;
			next.challenging = false;
			switch_allowed = false;
		} else {
			next.failed_probes = 0;
			next.metric = current->metric;
			if (best != NULL && memcmp(best->id, next.selected_id, 16) != 0 &&
				margin_met(best->metric, next.metric)) {
				if (!next.challenging || memcmp(best->id, next.challenger_id, 16) != 0) {
					next.challenging = true;
					memcpy(next.challenger_id, best->id, 16);
					next.challenger_since_seconds = now;
				}
				switch_allowed = now >= next.challenger_since_seconds &&
								 now - next.challenger_since_seconds >= MESH_HOLD_SECONDS;
			} else {
				next.challenging = false;
				switch_allowed = false;
			}
		}
	}
	if (switch_allowed) {
		next.selected = best != NULL;
		if (best != NULL) {
			memcpy(next.selected_id, best->id, 16);
			next.metric = best->metric;
		} else {
			memset(next.selected_id, 0, sizeof(next.selected_id));
			next.metric = 0;
		}
		action.identity_changed = had_selection != next.selected ||
								  (next.selected && memcmp(previous, next.selected_id, 16) != 0);
		if (action.identity_changed) {
			next.last_switch_seconds = now;
			next.failed_probes = 0;
			next.challenging = false;
		}
	}
	/* No udhcpc spawn, and no renew for a mere MRP/MBP attribute change.
	 * Losing all portals does not by itself renew into nowhere. */
	action.renew_netifd_client = local_role == MESH_ROLE_PEER &&
							   action.identity_changed && next.selected;
	*state = next;
	*result = action;
	return MESH_OK;
}
