/**
 * @file services_transaction.c
 * @brief Order role-service apply, compensation, and restoration.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "services.h"
#include "mrp.h"
#include <string.h>

static bool success(enum mesh_runtime_result result)
{
	return result == MESH_RUNTIME_OK || result == MESH_RUNTIME_UNCHANGED;
}

enum mesh_runtime_result mesh_peer_managed_stop(struct mesh_peer *peer, int root,
												 struct mesh_native_config *config,
												 const struct mesh_plan *plan,
												 struct mesh_runtime_transaction *transaction)
{
	struct mesh_runtime_data held = {0};
	volatile sig_atomic_t *cancelled = peer->cancelled;
	enum mesh_runtime_result result = MESH_RUNTIME_ROLLBACK_NEEDED;
	bool previous = config->hold_mesh;

	peer->cancelled = NULL;
	config->hold_mesh = true;
	peer->config = config;
	if (peer->services == NULL || (!peer->services->journal_exists && !transaction->active)) {
		result = MESH_RUNTIME_UNCHANGED;
		goto done;
	}
	if (transaction->active && !transaction->data_restored) {
		if (mesh_runtime_payload(config, plan, &held) != MESH_OK ||
			!success(mesh_runtime_withdraw(root, peer->directory, config, plan, &held, &peer->ops, transaction)) ||
			(plan->role == MESH_ROLE_MRP && peer->services->disk.dns_state != 0 &&
			 mesh_mrp_dns_profile(peer->services, false) != 0) || mesh_services_down(peer) != 0) {
			goto done;
		}
	}
	if (transaction->active && !success(mesh_runtime_stop(peer->directory, &peer->ops, transaction))) {
		goto done;
	}
	if (peer->ops.verify(peer, plan) != 0 || mesh_services_restore(peer) != 0) {
		goto done;
	}
	result = MESH_RUNTIME_OK;
done:
	config->hold_mesh = previous;
	peer->cancelled = cancelled;
	mesh_runtime_data_clear(&held);
	return result;
}

enum mesh_runtime_result mesh_peer_managed_apply(struct mesh_peer *peer, int root,
												  struct mesh_native_config *config,
												  const struct mesh_plan *plan,
												  volatile sig_atomic_t *cancelled,
												  struct mesh_runtime_transaction *transaction)
{
	struct mesh_runtime_data wanted = {0};
	enum mesh_runtime_result result = MESH_RUNTIME_INVALID;
	bool previous = config->hold_mesh;
	if (peer->services == NULL || !config->manage_services) {
		return result;
	}
	peer->config = config;
	peer->plan = plan;
	if (peer->services->recovering) {
		return MESH_RUNTIME_STALE_JOURNAL;
	}
	if (transaction->active && !config->hold_mesh) {
		if (peer->ops.read(peer, &wanted) == 0 && wanted.length == transaction->installed.length &&
			memcmp(wanted.bytes, transaction->installed.bytes, wanted.length) == 0 &&
			mesh_services_verify(peer) == 0 && peer->ops.verify(peer, plan) == 0) {
			result = MESH_RUNTIME_UNCHANGED;
		} else {
			result = MESH_RUNTIME_CONFLICT;
		}
		mesh_runtime_data_clear(&wanted);
		return result;
	}
	if (cancelled != NULL && *cancelled) {
		return MESH_RUNTIME_CANCELLED;
	}
	if (mesh_services_prepare(peer) != 0) {
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
		goto failed;
	}
	config->hold_mesh = true;
	if (mesh_runtime_payload(config, plan, &wanted) != MESH_OK) {
		goto failed;
	}
	result = mesh_runtime_apply(root, peer->directory, config, plan, &wanted, &peer->ops,
								cancelled, false, transaction);
	if (!success(result)) {
		goto failed;
	}
	if ((cancelled != NULL && *cancelled) || mesh_services_activate(peer) != 0) {
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
		goto failed;
	}
	if (plan->role == MESH_ROLE_MRP &&
		(peer->transport.reload_firewall == NULL || peer->transport.reload_firewall(peer->transport.context) != 0 ||
		 mesh_mrp_firewall_verify(peer, false) != 0 || mesh_mrp_dns_profile(peer->services, true) != 0 ||
		 mesh_mrp_verify(peer) != 0)) {
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
		goto failed;
	}
	config->hold_mesh = false;
	if (mesh_runtime_payload(config, plan, &wanted) != MESH_OK) {
		result = MESH_RUNTIME_INVALID;
		goto failed;
	}
	result = mesh_runtime_promote(root, peer->directory, config, plan, &wanted, &peer->ops, transaction);
	if (!success(result)) {
		goto failed;
	}
	goto done;
failed:
	if (!success(mesh_peer_managed_stop(peer, root, config, plan, transaction))) {
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
	}
done:
	config->hold_mesh = previous;
	mesh_runtime_data_clear(&wanted);
	return result;
}
