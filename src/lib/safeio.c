/**
 * @file safeio.c
 * @brief Read files inside a bounded root and fingerprint their bytes.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "native.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/openat2.h>

void mesh_sensitive_clear(void *memory, size_t length)
{
	volatile unsigned char *cursor;

	cursor = memory;
	while (length != 0) {
		*cursor = 0;
		cursor++;
		length--;
	}
}

static bool safe_relative(const char *path)
{
	const char *component;
	const char *end;
	size_t length;

	if (path == NULL || path[0] == '\0' || path[0] == '/') {
		return false;
	}
	component = path;
	do {
		end = strchr(component, '/');
		length = end != NULL ? (size_t)(end - component) : strlen(component);
		if (length == 0 || (length == 1 && component[0] == '.') ||
			(length == 2 && memcmp(component, "..", 2) == 0)) {
			return false;
		}
		component = end != NULL ? end + 1 : NULL;
	} while (component != NULL);
	return true;
}

/* Matching 25.12 kernels provide openat2. Fail closed on unsupported kernels;
 * never fall back to escaping fixture roots via intermediate symlinks.
 * RESOLVE_IN_ROOT still permits genuine sysfs class/device symlinks. */
int mesh_open_at_root(int root_fd, const char *path, int flags)
{
	struct open_how how;

	if (!safe_relative(path)) {
		errno = EINVAL;
		return -1;
	}
	memset(&how, 0, sizeof(how));
	how.flags = (unsigned int)flags;
	how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;
	return (int)syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
}

ssize_t mesh_readlink_at_root(int root_fd, const char *path, char *buffer, size_t capacity)
{
	int fd;
	ssize_t result;
	fd = mesh_open_at_root(root_fd, path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	result = readlinkat(fd, "", buffer, capacity);
	close(fd);
	return result;
}

static bool same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
		   a->st_size == b->st_size && a->st_mode == b->st_mode &&
		   a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
		   a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		   a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

int mesh_read_bounded_at(int root_fd, const char *path, void *buffer, size_t capacity,
						size_t *length, struct mesh_file_stamp *stamp)
{
	struct stat before;
	struct stat after;
	unsigned char *bytes;
	unsigned char extra;
	size_t used;
	ssize_t count;
	int fd;
	int result;

	if (!safe_relative(path) || buffer == NULL || capacity == 0 || length == NULL ||
		(stamp != NULL && strlen(path) >= sizeof(stamp->relative_path))) {
		return -1;
	}
	fd = mesh_open_at_root(root_fd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}
	result = -1;
	used = 0;
	bytes = buffer;
	if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0) {
		goto done;
	}
	while (used < capacity) {
		count = read(fd, bytes + used, capacity - used);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0) {
			goto done;
		}
		if (count == 0) {
			break;
		}
		used += (size_t)count;
	}
	do {
		count = read(fd, &extra, 1);
	} while (count < 0 && errno == EINTR);
	if (count != 0 || fstat(fd, &after) < 0 || !same_file(&before, &after)) {
		goto done;
	}
	if (stamp != NULL) {
		memset(stamp, 0, sizeof(*stamp));
		memcpy(stamp->relative_path, path, strlen(path) + 1);
		stamp->device = before.st_dev;
		stamp->inode = before.st_ino;
		stamp->size = before.st_size;
		stamp->mode = before.st_mode;
		stamp->uid = before.st_uid;
		stamp->gid = before.st_gid;
		stamp->mtime = before.st_mtim;
		stamp->ctime = before.st_ctim;
		sha256_hex(buffer, used, stamp->content_hash);
	}
	*length = used;
	result = 0;
done:
	close(fd);
	if (result != 0) {
		mesh_sensitive_clear(buffer, capacity);
	}
	return result;
}

int mesh_stamp_unchanged(int root_fd, const struct mesh_file_stamp *stamp)
{
	struct mesh_file_stamp current;
	unsigned char bytes[MESH_INPUT_MAX];
	size_t length;
	int result;

	if (stamp == NULL || !safe_relative(stamp->relative_path)) {
		return -1;
	}
	result = mesh_read_bounded_at(root_fd, stamp->relative_path, bytes, sizeof(bytes), &length, &current);
	if (result == 0) {
		result = current.device == stamp->device && current.inode == stamp->inode &&
				 current.size == stamp->size && current.mode == stamp->mode &&
				 current.uid == stamp->uid && current.gid == stamp->gid &&
				 current.mtime.tv_sec == stamp->mtime.tv_sec && current.mtime.tv_nsec == stamp->mtime.tv_nsec &&
				 current.ctime.tv_sec == stamp->ctime.tv_sec && current.ctime.tv_nsec == stamp->ctime.tv_nsec &&
				 memcmp(current.content_hash, stamp->content_hash, sizeof(stamp->content_hash)) == 0 ? 0 : -1;
	}
	mesh_sensitive_clear(&current, sizeof(current));
	mesh_sensitive_clear(bytes, sizeof(bytes));
	return result;
}
