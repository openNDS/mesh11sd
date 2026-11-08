/**
 * @file transaction.c
 * @brief Compare, replace, reload, and verify one owned runtime record.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static bool equal(const struct mesh_runtime_data *a, const struct mesh_runtime_data *b)
{
	return a->length <= MESH_RUNTIME_BYTES && a->length == b->length &&
		   memcmp(a->bytes, b->bytes, a->length) == 0;
}

static bool valid_ops(const struct mesh_runtime_ops *ops)
{
	return ops != NULL && ops->read != NULL && ops->replace != NULL &&
		   ops->reload != NULL && ops->verify != NULL;
}

static int guard(int root, const struct mesh_native_config *config)
{
	size_t i;

	if (config->stamp_count == 0 || config->stamp_count > MESH_INPUT_FILES) {
		return -1;
	}
	for (i = 0; i < config->stamp_count; i++) {
		if (mesh_stamp_unchanged(root, &config->stamps[i]) != 0) {
			return -1;
		}
	}
	return 0;
}

static bool cancelled_now(volatile sig_atomic_t *cancelled)
{
	return cancelled != NULL && *cancelled != 0;
}

static int journal(int directory, const char *phase, const struct mesh_native_config *config)
{
	struct stat metadata;
	char text[1024];
	size_t used;
	size_t i;
	ssize_t count;
	int fd;
	int written;

	if (directory < 0 || fstat(directory, &metadata) < 0 || !S_ISDIR(metadata.st_mode) ||
		(metadata.st_mode & 0077) != 0 || metadata.st_uid != geteuid()) {
		return -1;
	}
	used = 0;
	written = snprintf(text, sizeof(text), "mesh11sd-v8-transaction 1\nphase=%s\n", phase);
	if (written < 0 || (size_t)written >= sizeof(text)) {
		return -1;
	}
	used = (size_t)written;
	/* Metadata only: no key, identifier, config value, old payload, or hash. */
	for (i = 0; config != NULL && i < config->stamp_count; i++) {
		written = snprintf(text + used, sizeof(text) - used,
						   "input=%u inode=%ju size=%jd mode=%jo uid=%ju gid=%ju mtime=%jd:%ld\n",
						   (unsigned int)i, (uintmax_t)config->stamps[i].inode,
						   (intmax_t)config->stamps[i].size, (uintmax_t)config->stamps[i].mode,
						   (uintmax_t)config->stamps[i].uid, (uintmax_t)config->stamps[i].gid,
						   (intmax_t)config->stamps[i].mtime.tv_sec, config->stamps[i].mtime.tv_nsec);
		if (written < 0 || (size_t)written >= sizeof(text) - used) {
			return -1;
		}
		used += (size_t)written;
	}
	fd = openat(directory, "transaction.pending", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	count = write(fd, text, used);
	written = count == (ssize_t)used && fsync(fd) == 0 ? 0 : -1;
	if (close(fd) < 0) {
		written = -1;
	}
	if (written < 0) {
		unlinkat(directory, "transaction.pending", 0);
		return -1;
	}
	/* Only our private journal is replaced. Committed config is never written. */
	if (renameat(directory, "transaction.pending", directory, "transaction.meta") < 0 || fsync(directory) < 0) {
		unlinkat(directory, "transaction.pending", 0);
		return -1;
	}
	return 0;
}

int mesh_runtime_record_restored(int directory)
{
	return journal(directory, "restored", NULL);
}

void mesh_runtime_transaction_clear(struct mesh_runtime_transaction *transaction)
{
	if (transaction != NULL) {
		mesh_sensitive_clear(transaction, sizeof(*transaction));
	}
}

static enum mesh_runtime_result rollback(int directory, const struct mesh_runtime_ops *ops,
										 struct mesh_runtime_transaction *transaction)
{
	struct mesh_runtime_data current;
	enum mesh_runtime_result result;

	memset(&current, 0, sizeof(current));
	result = MESH_RUNTIME_ROLLBACK_NEEDED;
	if (ops->read(ops->context, &current) != 0 ||
		!equal(&current, transaction->data_restored ? &transaction->original : &transaction->installed)) {
		transaction->rollback_needed = true;
		goto done;
	}
	if (!transaction->data_restored) {
		if (ops->replace(ops->context, &current, &transaction->original) != 0) {
			transaction->rollback_needed = true;
			goto done;
		}
		transaction->data_restored = true;
	}
	if (ops->reload(ops->context) != 0 || ops->read(ops->context, &current) != 0 ||
		!equal(&current, &transaction->original)) {
		transaction->rollback_needed = true;
		goto done;
	}
	transaction->active = false;
	transaction->data_restored = false;
	transaction->rollback_needed = false;
	if (journal(directory, "restored", NULL) != 0) {
		transaction->active = true;
		transaction->data_restored = true;
		transaction->rollback_needed = true;
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
		goto done;
	}
	result = MESH_RUNTIME_OK;
done:
	mesh_runtime_data_clear(&current);
	return result;
}

static int transaction_lock(int directory)
{
	struct stat metadata;
	int fd;

	if (directory < 0 || fstat(directory, &metadata) < 0 || !S_ISDIR(metadata.st_mode) ||
		metadata.st_uid != geteuid() || (metadata.st_mode & 0077) != 0) {
		return -1;
	}
	fd = openat(directory, "transaction.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
		metadata.st_uid != geteuid() || (metadata.st_mode & 0077) != 0 ||
		flock(fd, LOCK_EX | LOCK_NB) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* A crash loses confidential rollback payloads intentionally kept in RAM.
 * Do not replace an unresolved prior owner's journal or adopt its runtime data. */
static bool journal_safe_to_begin(int directory)
{
	static const char restored[] = "mesh11sd-v8-transaction 1\nphase=restored\n";
	struct stat metadata;
	struct mesh_file_stamp stamp;
	char contents[1024];
	size_t length;

	if (fstatat(directory, "transaction.pending", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
		return false;
	}
	if (fstatat(directory, "transaction.meta", &metadata, AT_SYMLINK_NOFOLLOW) < 0) {
		return errno == ENOENT;
	}
	if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
		metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0600 ||
		mesh_read_bounded_at(directory, "transaction.meta", contents, sizeof(contents), &length, &stamp) != 0) {
		return false;
	}
	return length == sizeof(restored) - 1 && memcmp(contents, restored, length) == 0;
}

enum mesh_runtime_result mesh_runtime_stop(int journal_directory_fd,
										   const struct mesh_runtime_ops *ops,
										   struct mesh_runtime_transaction *transaction)
{
	enum mesh_runtime_result result;
	int fd;

	if (!valid_ops(ops) || transaction == NULL || !transaction->prepared) {
		return MESH_RUNTIME_INVALID;
	}
	if (!transaction->active) {
		return transaction->rollback_needed ? MESH_RUNTIME_ROLLBACK_NEEDED : MESH_RUNTIME_UNCHANGED;
	}
	fd = transaction_lock(journal_directory_fd);
	if (fd < 0) {
		return MESH_RUNTIME_CONFLICT;
	}
	result = rollback(journal_directory_fd, ops, transaction);
	flock(fd, LOCK_UN);
	close(fd);
	return result;
}

static enum mesh_runtime_result promote(int root, int directory,
											 const struct mesh_native_config *config,
											 const struct mesh_plan *plan,
											 const struct mesh_runtime_data *wanted,
											 const struct mesh_runtime_ops *ops,
											 struct mesh_runtime_transaction *transaction,
											 bool guard_inputs)
{
	struct mesh_runtime_data observed = {0};
	struct mesh_runtime_data validated = {0};
	struct mesh_runtime_data previous = {0};
	enum mesh_runtime_result result = MESH_RUNTIME_CONFLICT;
	int replaced;
	int lock;

	if (!valid_ops(ops) || config == NULL || plan == NULL || wanted == NULL || transaction == NULL ||
		!transaction->prepared || !transaction->active || transaction->rollback_needed ||
		mesh_runtime_payload(config, plan, &validated) != MESH_OK || !equal(wanted, &validated)) {
		mesh_runtime_data_clear(&validated);
		return MESH_RUNTIME_INVALID;
	}
	mesh_runtime_data_clear(&validated);
	lock = transaction_lock(directory);
	if (lock < 0) {
		return result;
	}
	if ((guard_inputs && guard(root, config) != 0) || ops->read(ops->context, &observed) != 0 ||
		!equal(&observed, &transaction->installed)) {
		goto done;
	}
	if (equal(&observed, wanted)) {
		result = ops->verify(ops->context, plan) == 0 ? MESH_RUNTIME_UNCHANGED : MESH_RUNTIME_ROLLBACK_NEEDED;
		goto done;
	}
	previous = transaction->installed;
	if (journal(directory, "prepared", config) != 0 || (guard_inputs && guard(root, config) != 0)) {
		goto done;
	}
	replaced = ops->replace(ops->context, &previous, wanted);
	if (replaced != 0) {
		if (replaced == -2 && ops->read(ops->context, &observed) == 0 && equal(&observed, wanted)) {
			transaction->installed = *wanted;
		}
		result = MESH_RUNTIME_ROLLBACK_NEEDED;
		goto done;
	}
	transaction->installed = *wanted;
	/* Ordered owner cleanup is mandatory: do NOT restore LAN servers while
	 * a possibly active mesh is still attached to their bridge. */
	result = MESH_RUNTIME_ROLLBACK_NEEDED;
	if ((guard_inputs && guard(root, config) != 0) || journal(directory, "published", config) != 0 ||
		ops->reload(ops->context) != 0 || (guard_inputs && guard(root, config) != 0) ||
		ops->verify(ops->context, plan) != 0 || ops->read(ops->context, &observed) != 0 ||
		!equal(&observed, wanted) || journal(directory, "verified", config) != 0) {
		goto done;
	}
	result = MESH_RUNTIME_OK;
done:
	mesh_runtime_data_clear(&previous);
	mesh_runtime_data_clear(&observed);
	flock(lock, LOCK_UN);
	close(lock);
	return result;
}

enum mesh_runtime_result mesh_runtime_promote(int root, int directory,
											 const struct mesh_native_config *config,
											 const struct mesh_plan *plan,
											 const struct mesh_runtime_data *wanted,
											 const struct mesh_runtime_ops *ops,
											 struct mesh_runtime_transaction *transaction)
{
	return promote(root, directory, config, plan, wanted, ops, transaction, true);
}

enum mesh_runtime_result mesh_runtime_withdraw(int root, int directory,
											  const struct mesh_native_config *config,
											  const struct mesh_plan *plan,
											  const struct mesh_runtime_data *wanted,
											  const struct mesh_runtime_ops *ops,
											  struct mesh_runtime_transaction *transaction)
{
	/* Cleanup may hold OUR unchanged payload after committed input changes.
	 * It cannot enable mesh, change the ownership check or adopt foreign data. */
	if (config == NULL || !config->hold_mesh) {
		return MESH_RUNTIME_INVALID;
	}
	return promote(root, directory, config, plan, wanted, ops, transaction, false);
}

enum mesh_runtime_result mesh_runtime_apply(int root_fd, int journal_directory_fd,
											const struct mesh_native_config *config,
											const struct mesh_plan *plan,
											const struct mesh_runtime_data *wanted,
											const struct mesh_runtime_ops *ops,
											volatile sig_atomic_t *cancelled,
											bool dry_run,
											struct mesh_runtime_transaction *transaction)
{
	struct mesh_runtime_data observed;
	struct mesh_runtime_data validated;
	bool valid_payload;
	enum mesh_runtime_result result;
	int fd;
	int replaced;

	if (config == NULL || plan == NULL || wanted == NULL || wanted->length == 0 ||
		wanted->length > MESH_RUNTIME_BYTES || !valid_ops(ops) || transaction == NULL) {
		return MESH_RUNTIME_INVALID;
	}
	memset(&validated, 0, sizeof(validated));
	valid_payload = mesh_runtime_payload(config, plan, &validated) == MESH_OK && equal(wanted, &validated);
	mesh_runtime_data_clear(&validated);
	if (!valid_payload) {
		return MESH_RUNTIME_INVALID;
	}
	if (guard(root_fd, config) != 0) {
		return MESH_RUNTIME_CHANGED_INPUT;
	}
	if (cancelled_now(cancelled)) {
		return MESH_RUNTIME_CANCELLED;
	}
	/* Dry run creates no lock, journal, payload backup, service or network state. */
	if (dry_run) {
		return MESH_RUNTIME_DRY_RUN;
	}
	fd = transaction_lock(journal_directory_fd);
	if (fd < 0) {
		return MESH_RUNTIME_CONFLICT;
	}
	memset(&observed, 0, sizeof(observed));
	result = MESH_RUNTIME_INVALID;
	if (!transaction->prepared && !journal_safe_to_begin(journal_directory_fd)) {
		result = MESH_RUNTIME_STALE_JOURNAL;
		goto done;
	}
	if (ops->read(ops->context, &observed) != 0 || observed.length > MESH_RUNTIME_BYTES) {
		goto done;
	}
	if (transaction->active) {
		if (!equal(&observed, &transaction->installed)) {
			result = MESH_RUNTIME_CONFLICT;
			goto done;
		}
		if (equal(&observed, wanted)) {
			result = ops->verify(ops->context, plan) == 0 ? MESH_RUNTIME_UNCHANGED : MESH_RUNTIME_INVALID;
			goto done;
		}
		/* Don't silently lose the original rollback target on reconfiguration. */
		result = MESH_RUNTIME_CONFLICT;
		goto done;
	}
	if (equal(&observed, wanted)) {
		/* Matching bytes alone do not establish ownership after restart. */
		result = MESH_RUNTIME_CONFLICT;
		goto done;
	}
	transaction->original = observed;
	transaction->installed = *wanted;
	transaction->prepared = true;
	if (journal(journal_directory_fd, "prepared", config) != 0 || guard(root_fd, config) != 0 ||
		cancelled_now(cancelled)) {
		result = cancelled_now(cancelled) ? MESH_RUNTIME_CANCELLED : MESH_RUNTIME_CHANGED_INPUT;
		goto done;
	}
	replaced = ops->replace(ops->context, &observed, wanted);
	if (replaced == -2) {
		transaction->active = true;
		result = rollback(journal_directory_fd, ops, transaction) == MESH_RUNTIME_OK ?
				 MESH_RUNTIME_FAILED_ROLLED_BACK : MESH_RUNTIME_ROLLBACK_NEEDED;
		goto done;
	}
	if (replaced != 0) {
		result = MESH_RUNTIME_CONFLICT;
		goto done;
	}
	transaction->active = true;
	if (guard(root_fd, config) != 0 || cancelled_now(cancelled) ||
		journal(journal_directory_fd, "published", config) != 0 ||
		ops->reload(ops->context) != 0 || guard(root_fd, config) != 0 ||
		cancelled_now(cancelled) || ops->verify(ops->context, plan) != 0 ||
		ops->read(ops->context, &observed) != 0 || !equal(&observed, wanted) ||
		journal(journal_directory_fd, "verified", config) != 0) {
		result = rollback(journal_directory_fd, ops, transaction) == MESH_RUNTIME_OK ?
				 MESH_RUNTIME_FAILED_ROLLED_BACK : MESH_RUNTIME_ROLLBACK_NEEDED;
		goto done;
	}
	result = MESH_RUNTIME_OK;
done:
	mesh_runtime_data_clear(&observed);
	flock(fd, LOCK_UN);
	close(fd);
	return result;
}
