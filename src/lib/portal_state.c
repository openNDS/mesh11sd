/**
 * @file portal_state.c
 * @brief Store the public-identity neighbor journal before each mutation.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "portal_runtime.h"
#include <errno.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define JOURNAL_SIZE 96
static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void pack_neighbor(uint8_t *p, const struct mesh_neighbor *neighbor)
{
	p[0] = neighbor->exists ? 1 : 0;
	memcpy(p + 1, neighbor->mac, 6);
	p[7] = (uint8_t)(neighbor->state >> 8);
	p[8] = (uint8_t)neighbor->state;
	p[9] = neighbor->flags;
	p[10] = neighbor->type;
}
static int unpack_neighbor(const uint8_t *p, struct mesh_neighbor *neighbor)
{
	static const uint8_t zero[16];
	if (p[0] > 1 || (p[0] == 0 && memcmp(p, zero, 11) != 0)) {
		return -1;
	}
	memset(neighbor, 0, sizeof(*neighbor));
	neighbor->exists = p[0] != 0;
	memcpy(neighbor->mac, p + 1, 6);
	neighbor->state = (uint16_t)((unsigned int)p[7] * 256 + p[8]);
	neighbor->flags = p[9];
	neighbor->type = p[10];
	if (neighbor->exists && neighbor->state > 255) {
		return -1;
	}
	return 0;
}
static bool owned_pin(const struct mesh_neighbor *neighbor)
{
	return neighbor->exists && neighbor->state == NUD_PERMANENT && neighbor->flags == 0 &&
		   neighbor->type == RTN_UNICAST && mesh_mac_valid(neighbor->mac);
}
static bool previous_valid(const struct mesh_neighbor *neighbor)
{
	static const uint8_t zero[6];
	return !neighbor->exists || (neighbor->flags == 0 && neighbor->type == RTN_UNICAST &&
		   (neighbor->state & (NUD_PERMANENT | NUD_NOARP)) == 0 &&
		   (mesh_mac_valid(neighbor->mac) || (memcmp(neighbor->mac, zero, 6) == 0 &&
			(neighbor->state & ~(NUD_INCOMPLETE | NUD_FAILED)) == 0)));
}
static bool journal_valid(const struct mesh_portal_runtime *runtime)
{
	const struct mesh_neighbor *previous = &runtime->previous;
	const struct mesh_neighbor *installed = &runtime->installed;
	const struct mesh_neighbor *pending = &runtime->pending;
	static const uint8_t zero[16];
	if (!previous_valid(previous) ||
		(runtime->renew_uncertain && memcmp(runtime->notified, zero, 16) == 0)) {
		return false;
	}
	if (runtime->phase == 0) {
		return !previous->exists && !installed->exists && !pending->exists;
	}
	if (runtime->phase == 1) {
		return owned_pin(pending) && (owned_pin(installed) ||
			   (installed->exists == previous->exists && installed->state == previous->state &&
				installed->flags == previous->flags && installed->type == previous->type &&
				memcmp(installed->mac, previous->mac, 6) == 0));
	}
	return (runtime->phase == 2 || runtime->phase == 3) && owned_pin(installed) && !pending->exists;
}
static bool safe_metadata(const struct mesh_file_stamp *stamp)
{
	return stamp->uid == geteuid() && S_ISREG(stamp->mode) && (stamp->mode & 0777) == 0600;
}
int mesh_portal_journal_load(struct mesh_portal_runtime *runtime)
{
	uint8_t bytes[JOURNAL_SIZE];
	static const uint8_t zero[16];
	size_t size;
	struct stat metadata;
	if (fstatat(runtime->directory, "portal.pending", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
		return -1;
	}
	if (fstatat(runtime->directory, "portal.meta", &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
		(!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)) {
		return -1;
	}
	if (mesh_read_bounded_at(runtime->directory, "portal.meta", bytes, sizeof(bytes), &size, &runtime->stamp) != 0) {
		return errno == ENOENT ? 0 : -1;
	}
	if (size != sizeof(bytes) || !safe_metadata(&runtime->stamp) || memcmp(bytes, "M11PORT1", 8) != 0 ||
		bytes[8] > 3 || bytes[9] > 1 || bytes[10] != 0 || bytes[11] != 0 || memcmp(bytes + 85, zero, 11) != 0 ||
		get32(bytes + 12) == 0 || get32(bytes + 16) == 0 ||
		unpack_neighbor(bytes + 20, &runtime->previous) != 0 ||
		unpack_neighbor(bytes + 31, &runtime->installed) != 0 || unpack_neighbor(bytes + 42, &runtime->pending) != 0) {
		return -1;
	}
	runtime->phase = bytes[8];
	runtime->renew_uncertain = bytes[9] != 0;
	runtime->bridge_index = get32(bytes + 12);
	runtime->gateway = get32(bytes + 16);
	memcpy(runtime->selection.selected_id, bytes + 53, 16);
	memcpy(runtime->notified, bytes + 69, 16);
	runtime->selection.selected = memcmp(bytes + 53, zero, 16) != 0;
	runtime->pin_active = runtime->phase != 0;
	runtime->journal_exists = true;
	return journal_valid(runtime) ? 0 : -1;
}
int mesh_portal_journal_store(struct mesh_portal_runtime *runtime)
{
	uint8_t bytes[JOURNAL_SIZE] = {0};
	uint8_t check[JOURNAL_SIZE];
	struct stat metadata;
	size_t size;
	size_t written = 0;
	ssize_t result;
	int fd;
	if (!journal_valid(runtime)) {
		return -1;
	}
	if (runtime->journal_exists ? mesh_stamp_unchanged(runtime->directory, &runtime->stamp) != 0 :
		(fstatat(runtime->directory, "portal.meta", &metadata, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)) {
		return -1;
	}
	memcpy(bytes, "M11PORT1", 8);
	bytes[8] = runtime->phase;
	bytes[9] = runtime->renew_uncertain ? 1 : 0;
	put32(bytes + 12, runtime->bridge_index);
	put32(bytes + 16, runtime->gateway);
	pack_neighbor(bytes + 20, &runtime->previous);
	pack_neighbor(bytes + 31, &runtime->installed);
	pack_neighbor(bytes + 42, &runtime->pending);
	if (runtime->selection.selected) {
		memcpy(bytes + 53, runtime->selection.selected_id, 16);
	}
	memcpy(bytes + 69, runtime->notified, 16);
	fd = openat(runtime->directory, "portal.pending", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	while (written < sizeof(bytes)) {
		result = write(fd, bytes + written, sizeof(bytes) - written);
		if (result < 0 && errno == EINTR) {
			continue;
		}
		if (result <= 0) {
			close(fd);
			unlinkat(runtime->directory, "portal.pending", 0);
			return -1;
		}
		written += (size_t)result;
	}
	result = fsync(fd);
	if (close(fd) != 0) {
		result = -1;
	}
	if (result != 0 || (runtime->journal_exists && mesh_stamp_unchanged(runtime->directory, &runtime->stamp) != 0) ||
		renameat(runtime->directory, "portal.pending", runtime->directory, "portal.meta") != 0) {
		unlinkat(runtime->directory, "portal.pending", 0);
		return -1;
	}
	runtime->journal_exists = true;
	if (fsync(runtime->directory) != 0 || mesh_read_bounded_at(runtime->directory, "portal.meta", check, sizeof(check),
															 &size, &runtime->stamp) != 0 ||
		size != sizeof(check) || memcmp(bytes, check, size) != 0 || !safe_metadata(&runtime->stamp)) {
		return -1;
	}
	return 0;
}
