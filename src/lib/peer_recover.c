/**
 * @file peer_recover.c
 * @brief Recover a recognized same-configuration crash journal.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "peer.h"
#include "services.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>

static bool unsigned_field(const char **cursor, const char *prefix, int base, uintmax_t *value)
{
	char *end;
	const char *start;
	if (strncmp(*cursor, prefix, strlen(prefix)) != 0) {
		return false;
	}
	start = *cursor + strlen(prefix);
	if (*start < '0' || *start > '9') {
		return false;
	}
	errno = 0;
	*value = strtoumax(start, &end, base);
	if (end == start || errno != 0) {
		return false;
	}
	*cursor = end;
	return true;
}

static bool signed_field(const char **cursor, const char *prefix, intmax_t *value)
{
	char *end;
	const char *start;
	if (strncmp(*cursor, prefix, strlen(prefix)) != 0) {
		return false;
	}
	start = *cursor + strlen(prefix);
	if ((*start < '0' || *start > '9') && *start != '-') {
		return false;
	}
	errno = 0;
	*value = strtoimax(start, &end, 10);
	if (end == start || errno != 0) {
		return false;
	}
	*cursor = end;
	return true;
}

static bool journal_text(const char *contents, size_t length)
{
	static const char prefix[] = "mesh11sd-v8-transaction 1\nphase=";
	const char *cursor;
	const char *end;
	const char *phase;
	char canonical[256];
	uintmax_t index;
	uintmax_t inode;
	intmax_t size;
	uintmax_t mode;
	uintmax_t uid;
	uintmax_t gid;
	intmax_t seconds;
	intmax_t nanoseconds;
	const char *field;
	int written;
	size_t count = 0;
	bool restored;

	if (length < sizeof(prefix) - 1 || memchr(contents, '\0', length) != NULL ||
		memcmp(contents, prefix, sizeof(prefix) - 1) != 0) {
		return false;
	}
	phase = contents + sizeof(prefix) - 1;
	end = strchr(phase, '\n');
	if (end == NULL) {
		return false;
	}
	restored = (size_t)(end - phase) == 8 && memcmp(phase, "restored", 8) == 0;
	if (!restored && !((size_t)(end - phase) == 8 && memcmp(phase, "prepared", 8) == 0) &&
		!((size_t)(end - phase) == 9 && memcmp(phase, "published", 9) == 0) &&
		!((size_t)(end - phase) == 8 && memcmp(phase, "verified", 8) == 0)) {
		return false;
	}
	cursor = end + 1;
	while (*cursor != '\0') {
		end = strchr(cursor, '\n');
		if (end == NULL || (size_t)(end - cursor) >= sizeof(canonical) || count == MESH_INPUT_FILES) {
			return false;
		}
		field = cursor;
		if (!unsigned_field(&field, "input=", 10, &index) ||
			!unsigned_field(&field, " inode=", 10, &inode) ||
			!signed_field(&field, " size=", &size) ||
			!unsigned_field(&field, " mode=", 8, &mode) ||
			!unsigned_field(&field, " uid=", 10, &uid) ||
			!unsigned_field(&field, " gid=", 10, &gid) ||
			!signed_field(&field, " mtime=", &seconds) ||
			!signed_field(&field, ":", &nanoseconds) || field != end || index != count ||
			size < 0 || nanoseconds < 0 || nanoseconds >= 1000000000) {
			return false;
		}
		written = snprintf(canonical, sizeof(canonical), "input=%ju inode=%ju size=%jd mode=%jo uid=%ju gid=%ju mtime=%jd:%jd",
						   index, inode, size, mode, uid, gid, seconds, nanoseconds);
		if (written != end - cursor || memcmp(cursor, canonical, (size_t)written) != 0) {
			return false;
		}
		count++;
		cursor = end + 1;
	}
	return restored ? count == 0 : count != 0;
}

static int inspect_journal(int directory, const char *name)
{
	struct stat metadata;
	char contents[2048];
	size_t length;

	if (fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT ? 0 : -1;
	}
	if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_uid != geteuid() ||
		(metadata.st_mode & 0777) != 0600 ||
		mesh_read_bounded_at(directory, name, contents, sizeof(contents) - 1, &length, NULL) != 0) {
		return -1;
	}
	contents[length] = '\0';
	if (!journal_text(contents, length)) {
		return -1;
	}
	return strstr(contents, "\nphase=restored\n") != NULL ? 2 : 1;
}

static int preserve_pending(int directory)
{
	unsigned int index;
	char name[64];
	for (index = 0; index < 100; index++) {
		snprintf(name, sizeof(name), "transaction.interrupted.%u", index);
		if (syscall(SYS_renameat2, directory, "transaction.pending", directory, name, RENAME_NOREPLACE) == 0) {
			return fsync(directory);
		}
		if (errno != EEXIST) {
			return -1;
		}
	}
	return -1;
}

enum mesh_runtime_result mesh_peer_recover_detached(struct mesh_peer *peer, int root,
													const struct mesh_native_config *config)
{
	struct mesh_runtime_data current = {0};
	enum mesh_runtime_result result = MESH_RUNTIME_STALE_JOURNAL;
	int metadata;
	int pending;
	size_t i;

	if (peer == NULL || !peer->initialized || config == NULL || peer->session_lock < 0 ||
		peer->ops.read(peer, &current) != 0) {
		return MESH_RUNTIME_CONFLICT;
	}
	if (current.length != 0) {
		mesh_runtime_data_clear(&current);
		return MESH_RUNTIME_UNCHANGED;
	}
	mesh_runtime_data_clear(&current);
	metadata = inspect_journal(peer->directory, "transaction.meta");
	pending = inspect_journal(peer->directory, "transaction.pending");
	if (metadata < 0 || pending < 0) {
		return result;
	}
	if ((metadata == 0 || metadata == 2) && pending == 0) {
		return MESH_RUNTIME_UNCHANGED;
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(root, &config->stamps[i]) != 0) {
			return MESH_RUNTIME_CHANGED_INPUT;
		}
	}
	/* A crash after deleting our service but before reload leaves netifd's old
	 * runtime objects. Recognized metadata permits ONLY a committed reload,
	 * never adoption or direct deletion of a possibly foreign kernel object. */
	peer->recovery_cleanup = true;
	if (mesh_peer_preflight(peer, config) != 0) {
		peer->recovery_cleanup = false;
		return MESH_RUNTIME_CONFLICT;
	}
	peer->recovery_cleanup = false;
	if (peer->transport.interfaces(peer->transport.context, &peer->baseline) != MESH_OK) {
		return MESH_RUNTIME_ROLLBACK_NEEDED;
	}
	peer->have_baseline = true;
	if (pending != 0 && preserve_pending(peer->directory) != 0) {
		return result;
	}
	if (peer->ops.reload(peer) != 0 || peer->ops.read(peer, &current) != 0 || current.length != 0) {
		mesh_runtime_data_clear(&current);
		return MESH_RUNTIME_ROLLBACK_NEEDED;
	}
	mesh_runtime_data_clear(&current);
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(root, &config->stamps[i]) != 0) {
			return MESH_RUNTIME_CHANGED_INPUT;
		}
	}
	if (mesh_runtime_record_restored(peer->directory) != 0) {
		return MESH_RUNTIME_ROLLBACK_NEEDED;
	}
	peer->have_baseline = false;
	return MESH_RUNTIME_OK;
}

enum mesh_runtime_result mesh_peer_recover(struct mesh_peer *peer, int root,
										   const struct mesh_native_config *config,
										   const struct mesh_plan *plan,
										   const struct mesh_runtime_data *wanted)
{
	struct mesh_runtime_data current;
	struct mesh_runtime_transaction transaction;
	enum mesh_runtime_result result = MESH_RUNTIME_STALE_JOURNAL;
	int metadata;
	int pending;
	size_t i;

	memset(&current, 0, sizeof(current));
	memset(&transaction, 0, sizeof(transaction));
	if (peer == NULL || !peer->initialized || config == NULL || plan == NULL || wanted == NULL ||
		peer->session_lock < 0 || peer->ops.read(peer, &current) != 0) {
		return MESH_RUNTIME_CONFLICT;
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(root, &config->stamps[i]) != 0) {
			result = MESH_RUNTIME_CHANGED_INPUT;
			goto done;
		}
	}
	metadata = inspect_journal(peer->directory, "transaction.meta");
	pending = inspect_journal(peer->directory, "transaction.pending");
	if (metadata < 0 || pending < 0) {
		goto done;
	}
	if (metadata == 0 && pending == 0) {
		result = current.length == 0 ? MESH_RUNTIME_UNCHANGED : MESH_RUNTIME_CONFLICT;
		goto done;
	}
	/* Never guess old credential/configuration data after a changed-config
	 * restart. Only an exact reconstructable payload plus retained owner ID
	 * establishes that the old service is still ours and unchanged. */
	if (current.length != 0 && (current.length != wanted->length ||
		memcmp(current.bytes, wanted->bytes, current.length) != 0)) {
		result = MESH_RUNTIME_CONFLICT;
		goto done;
	}
	peer->config = config;
	peer->plan = plan;
	if (pending != 0) {
		/* Known metadata only, owned private directory and lifetime lock.
		 * Preserve the interrupted record; never overwrite an unknown file. */
		if (preserve_pending(peer->directory) != 0) {
			goto done;
		}
	}
	if (current.length == 0) {
		if (peer->ops.reload(peer) != 0 || peer->ops.read(peer, &current) != 0 || current.length != 0 ||
			mesh_runtime_record_restored(peer->directory) != 0) {
			result = MESH_RUNTIME_ROLLBACK_NEEDED;
			goto done;
		}
		result = MESH_RUNTIME_OK;
		goto done;
	}
	transaction.installed = current;
	transaction.prepared = true;
	transaction.active = true;
	result = mesh_runtime_stop(peer->directory, &peer->ops, &transaction);
done:
	mesh_runtime_data_clear(&current);
	mesh_runtime_transaction_clear(&transaction);
	return result;
}

/* Same-config recovery accepts ONLY our exact final or mesh-held projection.
 * No confidential payload backup is written to disk. Unknown/changed state is
 * preserved; known interrupted metadata is retained by preserve_pending(). */
enum mesh_runtime_result mesh_peer_recover_managed(struct mesh_peer *peer, int root,
													struct mesh_native_config *config,
													const struct mesh_plan *plan)
{
	struct mesh_runtime_data current = {0};
	struct mesh_runtime_data final = {0};
	struct mesh_runtime_data held = {0};
	struct mesh_runtime_transaction transaction = {0};
	enum mesh_runtime_result result = MESH_RUNTIME_CONFLICT;
	bool previous = config->hold_mesh;
	int metadata;
	int pending;
	if (peer->services == NULL || peer->ops.read(peer, &current) != 0) {
		goto done;
	}
	if (current.length == 0) {
		result = mesh_peer_recover_detached(peer, root, config);
		if ((result == MESH_RUNTIME_OK || result == MESH_RUNTIME_UNCHANGED) && mesh_services_restore(peer) != 0) {
			result = MESH_RUNTIME_ROLLBACK_NEEDED;
		}
		goto done;
	}
	config->hold_mesh = false;
	if (mesh_runtime_payload(config, plan, &final) != MESH_OK) {
		goto done;
	}
	config->hold_mesh = true;
	if (mesh_runtime_payload(config, plan, &held) != MESH_OK ||
		!((current.length == final.length && memcmp(current.bytes, final.bytes, current.length) == 0) ||
		  (current.length == held.length && memcmp(current.bytes, held.bytes, current.length) == 0))) {
		goto done;
	}
	metadata = inspect_journal(peer->directory, "transaction.meta");
	pending = inspect_journal(peer->directory, "transaction.pending");
	if (metadata < 0 || pending < 0 || (metadata == 0 && pending == 0) ||
		!peer->services->journal_exists || (pending != 0 && preserve_pending(peer->directory) != 0)) {
		goto done;
	}
	transaction.installed = current;
	transaction.prepared = true;
	transaction.active = true;
	result = mesh_peer_managed_stop(peer, root, config, plan, &transaction);
done:
	config->hold_mesh = previous;
	mesh_runtime_data_clear(&current);
	mesh_runtime_data_clear(&final);
	mesh_runtime_data_clear(&held);
	mesh_runtime_transaction_clear(&transaction);
	return result;
}
