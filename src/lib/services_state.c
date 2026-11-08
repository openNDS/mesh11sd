/**
 * @file services_state.c
 * @brief Read and write the mode-only role-service journal.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "services.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool terminated(const char *value, size_t size)
{
	return memchr(value, '\0', size) != NULL;
}

int mesh_services_load(struct mesh_services *services)
{
	struct stat metadata;
	size_t length;
	size_t i;
	int directory = services->peer->directory;

	if (fstatat(directory, "services.pending", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
		return -1;
	}
	if (fstatat(directory, "services.meta", &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT ? 0 : -1;
	}
	if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
		(metadata.st_mode & 0777) != 0600 ||
		mesh_read_bounded_at(directory, "services.meta", &services->disk, sizeof(services->disk),
							 &length, &services->journal_stamp) != 0 || length != sizeof(services->disk)) {
		return -1;
	}
	if (services->disk.magic != MESH_SERVICE_MAGIC || services->disk.version != 2 ||
		services->disk.size != sizeof(services->disk) || services->disk.phase > 3 ||
		!terminated(services->disk.owner, sizeof(services->disk.owner)) ||
		strcmp(services->disk.owner, services->peer->owner) != 0 ||
		!terminated(services->disk.network, sizeof(services->disk.network)) ||
		!terminated(services->disk.bridge, sizeof(services->disk.bridge)) ||
		services->disk.dns_count > MESH_DNS_OWNERS_MAX || services->disk.was_up > 1 || services->disk.dns_state > 3) {
		return -1;
	}
	for (i = 0; i < sizeof(services->disk.reserved); i++) {
		if (services->disk.reserved[i] != 0) {
			return -1;
		}
	}
	for (i = 0; i < MESH_SERVICE_MODES; i++) {
		if (services->disk.modes[i] > 4) {
			return -1;
		}
	}
	for (i = 0; i < services->disk.dns_count; i++) {
		if (!terminated(services->disk.dns[i].instance, sizeof(services->disk.dns[i].instance)) ||
			!terminated(services->disk.dns[i].config, sizeof(services->disk.dns[i].config)) ||
			!terminated(services->disk.dns[i].directory, sizeof(services->disk.dns[i].directory))) {
			return -1;
		}
	}
	services->journal_exists = true;
	services->recovering = true;
	return 1;
}

int mesh_services_journal(struct mesh_services *services)
{
	size_t length;
	unsigned char check[sizeof(services->disk)];
	int directory = services->peer->directory;
	int fd;
	int result = -1;

	if (services->journal_exists && mesh_stamp_unchanged(directory, &services->journal_stamp) != 0) {
		return -1;
	}
	fd = openat(directory, "services.pending", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	if (write(fd, &services->disk, sizeof(services->disk)) == sizeof(services->disk) && fsync(fd) == 0) {
		result = 0;
	}
	if (close(fd) != 0) {
		result = -1;
	}
	if (result != 0) {
		unlinkat(directory, "services.pending", 0);
		return -1;
	}
	if (!services->journal_exists) {
		if (syscall(SYS_renameat2, directory, "services.pending", directory, "services.meta", RENAME_NOREPLACE) != 0) {
			return -1;
		}
	} else if (renameat(directory, "services.pending", directory, "services.meta") != 0) {
		return -1;
	}
	services->journal_exists = true;
	if (fsync(directory) != 0 || mesh_read_bounded_at(directory, "services.meta", check, sizeof(check),
													  &length, &services->journal_stamp) != 0 ||
		length != sizeof(check) || memcmp(check, &services->disk, sizeof(check)) != 0) {
		return -1;
	}
	return 0;
}

int mesh_services_remove_journal(struct mesh_services *services)
{
	int directory = services->peer->directory;
	if (!services->journal_exists) {
		return 0;
	}
	if (mesh_stamp_unchanged(directory, &services->journal_stamp) != 0 ||
		unlinkat(directory, "services.meta", 0) != 0 || fsync(directory) != 0) {
		return -1;
	}
	services->journal_exists = false;
	return 0;
}
