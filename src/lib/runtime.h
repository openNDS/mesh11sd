/**
 * @file runtime.h
 * @brief Compare, replace, reload, and verify transaction types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_RUNTIME_H
#define MESH_RUNTIME_H

#include "native.h"

#define MESH_RUNTIME_BYTES 32768

/* Confidential in-memory ubus blob. Never print, hash into logs, or persist. */
struct mesh_runtime_data {
	size_t length;
	unsigned char bytes[MESH_RUNTIME_BYTES];
};

enum mesh_runtime_result {
	MESH_RUNTIME_OK,
	MESH_RUNTIME_DRY_RUN,
	MESH_RUNTIME_UNCHANGED,
	MESH_RUNTIME_INVALID,
	MESH_RUNTIME_CHANGED_INPUT,
	MESH_RUNTIME_CONFLICT,
	MESH_RUNTIME_STALE_JOURNAL,
	MESH_RUNTIME_CANCELLED,
	MESH_RUNTIME_FAILED_ROLLED_BACK,
	MESH_RUNTIME_ROLLBACK_NEEDED
};

/* Owned backends hold an exclusive lifetime lock and compare private namespace
 * ownership/current data before mutation, then read back uncertain outcomes.
 * This is not cross-service CAS. Return -1 for known no-owned-change/conflict,
 * -2 for uncertain outcome requiring guarded compensation/recovery. */
struct mesh_runtime_ops {
	void *context;
	int (*read)(void *context, struct mesh_runtime_data *data);
	/* Compare expected state under backend ownership; never mutate unrelated
	 * services or resources. Return -2 if transport outcome cannot be resolved. */
	int (*replace)(void *context, const struct mesh_runtime_data *expected,
				   const struct mesh_runtime_data *replacement);
	int (*reload)(void *context);
	int (*verify)(void *context, const struct mesh_plan *plan);
};

struct mesh_runtime_transaction {
	struct mesh_runtime_data original;
	struct mesh_runtime_data installed;
	bool prepared;
	bool active;
	bool rollback_needed;
	bool data_restored;
};

enum mesh_error mesh_runtime_payload(const struct mesh_native_config *config,
									 const struct mesh_plan *plan,
									 struct mesh_runtime_data *output);
int mesh_runtime_record_restored(int directory);
void mesh_runtime_data_clear(struct mesh_runtime_data *data);
/* Call stop before clear; clearing memory does not undo active runtime state. */
void mesh_runtime_transaction_clear(struct mesh_runtime_transaction *transaction);
enum mesh_runtime_result mesh_runtime_apply(int root_fd, int journal_directory_fd,
											const struct mesh_native_config *config,
											const struct mesh_plan *plan,
											const struct mesh_runtime_data *wanted,
											const struct mesh_runtime_ops *ops,
											volatile sig_atomic_t *cancelled,
											bool dry_run,
											struct mesh_runtime_transaction *transaction);
/* Explicit private staged transition, retaining the ORIGINAL rollback target.
 * A failed promotion remains active for the owner's ordered compensation. */
enum mesh_runtime_result mesh_runtime_promote(int root_fd, int journal_directory_fd,
											 const struct mesh_native_config *config,
											 const struct mesh_plan *plan,
											 const struct mesh_runtime_data *wanted,
											 const struct mesh_runtime_ops *ops,
											 struct mesh_runtime_transaction *transaction);
/* Owned withdrawal only: permits safe mesh holding after source input changes. */
enum mesh_runtime_result mesh_runtime_withdraw(int root, int directory,
											  const struct mesh_native_config *config,
											  const struct mesh_plan *plan,
											  const struct mesh_runtime_data *wanted,
											  const struct mesh_runtime_ops *ops,
											  struct mesh_runtime_transaction *transaction);
enum mesh_runtime_result mesh_runtime_stop(int journal_directory_fd,
										   const struct mesh_runtime_ops *ops,
										   struct mesh_runtime_transaction *transaction);

#endif
