/**
 * @file wpad_guard.c
 * @brief Wait for a full wpad install before any mesh configuration is applied.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "wpad_guard.h"
#include "native.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MESH_WPAD_RECHECK_MS 60000u

struct mesh_wpad_name {
	const char *name;
	enum mesh_wpad_state state;
};

static const struct mesh_wpad_name mesh_wpad_names[] = {
	{ "wpad", MESH_WPAD_INTERNAL },
	{ "wpad-mbedtls", MESH_WPAD_FULL },
	{ "wpad-openssl", MESH_WPAD_FULL },
	{ "wpad-wolfssl", MESH_WPAD_FULL },
	{ "wpad-basic", MESH_WPAD_BASIC },
	{ "wpad-basic-mbedtls", MESH_WPAD_BASIC },
	{ "wpad-basic-openssl", MESH_WPAD_BASIC },
	{ "wpad-basic-wolfssl", MESH_WPAD_BASIC },
	{ "wpad-mini", MESH_WPAD_MINI },
	{ "wpad-mesh-mbedtls", MESH_WPAD_MESH_ONLY },
	{ "wpad-mesh-openssl", MESH_WPAD_MESH_ONLY },
	{ "wpad-mesh-wolfssl", MESH_WPAD_MESH_ONLY }
};

static int package_token(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0' || name[0] == '-' || name[0] == '.' || name[0] == '+') {
		return 0;
	}
	for (i = 0; name[i] != '\0'; i++) {
		unsigned char c = (unsigned char)name[i];
		if (!(islower(c) || isdigit(c) || c == '-' || c == '.' || c == '+') || i >= 64) {
			return 0;
		}
	}
	return 1;
}

static enum mesh_wpad_state name_state(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(mesh_wpad_names) / sizeof(mesh_wpad_names[0]); i++) {
		if (strcmp(name, mesh_wpad_names[i].name) == 0) {
			return mesh_wpad_names[i].state;
		}
	}
	return MESH_WPAD_ABSENT;
}

static int read_line(int fd, char *buffer, size_t capacity)
{
	size_t used = 0;
	char byte;
	ssize_t count;

	while (used + 1 < capacity) {
		count = read(fd, &byte, 1);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0) {
			return -1;
		}
		if (count == 0) {
			buffer[used] = '\0';
			return used == 0 ? 1 : 0;
		}
		if (byte == '\0') {
			return -1;
		}
		if (byte == '\n') {
			buffer[used] = '\0';
			return 0;
		}
		buffer[used] = byte;
		used++;
	}
	return -1;
}

static int field_line(const char *line)
{
	return line[0] != '\0' && line[1] == ':' && isalpha((unsigned char)line[0]);
}

static int multicall_matches(int root_fd, const char *package)
{
	char list_path[96];
	char line[256];
	char link[8];
	int list;
	int binary;
	int saw = 0;
	ssize_t length;
	int result = 0;

	if (snprintf(list_path, sizeof(list_path), "lib/apk/packages/%s.list", package) >= (int)sizeof(list_path)) {
		return 0;
	}
	list = mesh_open_at_root(root_fd, list_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (list < 0) {
		return 0;
	}
	while ((result = read_line(list, line, sizeof(line))) == 0) {
		if (strcmp(line, "/usr/sbin/wpad") == 0) {
			saw = 1;
		}
	}
	close(list);
	if (result < 0 || !saw) {
		return 0;
	}
	binary = mesh_open_at_root(root_fd, "usr/sbin/wpad", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (binary < 0) {
		return 0;
	}
	result = 0;
	if (fcntl(binary, F_GETFL) >= 0) {
		struct stat info;
		if (fstat(binary, &info) == 0 && S_ISREG(info.st_mode)) {
			result = 1;
		}
	}
	close(binary);
	if (!result) {
		return 0;
	}
	length = mesh_readlink_at_root(root_fd, "usr/sbin/hostapd", link, sizeof(link));
	if (length != 4 || memcmp(link, "wpad", 4) != 0) {
		return 0;
	}
	length = mesh_readlink_at_root(root_fd, "usr/sbin/wpa_supplicant", link, sizeof(link));
	if (length != 4 || memcmp(link, "wpad", 4) != 0) {
		return 0;
	}
	return 1;
}

enum mesh_wpad_state mesh_wpad_full_state(int root_fd)
{
	char line[512];
	int database;
	int line_result;
	int saw_full = 0;
	int full_matches = 0;
	enum mesh_wpad_state other = MESH_WPAD_ABSENT;

	if (root_fd < 0) {
		return MESH_WPAD_MALFORMED;
	}
	database = mesh_open_at_root(root_fd, "lib/apk/db/installed", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (database < 0) {
		return MESH_WPAD_ABSENT;
	}
	while ((line_result = read_line(database, line, sizeof(line))) == 0) {
		const char *name;
		enum mesh_wpad_state state;
		if (line[0] == '\0') {
			continue;
		}
		if (!field_line(line)) {
			close(database);
			return MESH_WPAD_MALFORMED;
		}
		if (line[0] != 'P') {
			continue;
		}
		name = line + 2;
		if (!package_token(name)) {
			close(database);
			return MESH_WPAD_MALFORMED;
		}
		state = name_state(name);
		if (state == MESH_WPAD_FULL) {
			saw_full = 1;
			if (multicall_matches(root_fd, name)) {
				full_matches = 1;
			}
		} else if (state != MESH_WPAD_ABSENT && other == MESH_WPAD_ABSENT) {
			other = state;
		}
	}
	close(database);
	if (line_result < 0) {
		return MESH_WPAD_MALFORMED;
	}
	if (saw_full) {
		return full_matches ? MESH_WPAD_FULL : MESH_WPAD_PARTIAL;
	}
	return other;
}

static const char *state_reason(enum mesh_wpad_state state)
{
	switch (state) {
	case MESH_WPAD_INTERNAL:
		return "unsuffixed wpad is the internal build and does not enable CONFIG_MESH or CONFIG_OWE";
	case MESH_WPAD_BASIC:
		return "installed wpad is basic (OWE without 802.11s mesh)";
	case MESH_WPAD_MINI:
		return "installed wpad is mini";
	case MESH_WPAD_MESH_ONLY:
		return "installed wpad is mesh-only and does not provide OWE";
	case MESH_WPAD_PARTIAL:
		return "full wpad record does not match the /usr/sbin/wpad multicall links";
	case MESH_WPAD_MALFORMED:
		return "apk installed database is malformed";
	default:
		return "no full wpad package is installed";
	}
}

static void log_state(struct mesh_wpad_time *time, enum mesh_wpad_state state)
{
	char line[512];
	int wrote;

	wrote = snprintf(line, sizeof(line),
					 "mesh11sd inactive: full wpad required for 802.11s and OWE "
					 "(wpad-mbedtls, wpad-openssl, or wpad-wolfssl); %s; "
					 "basic, mini, and mesh-only stay rejected",
					 state_reason(state));
	if (wrote < 0 || (size_t)wrote >= sizeof(line) || time->log_line == NULL) {
		return;
	}
	time->log_line(time->context, line);
}

static int stopped_without_reload(const struct mesh_wpad_time *time)
{
	return time->stop != NULL && *time->stop && (time->reload == NULL || !*time->reload);
}

static void consume_reload(struct mesh_wpad_time *time)
{
	if (time->reload != NULL && *time->reload) {
		*time->reload = 0;
		if (time->stop != NULL) {
			*time->stop = 0;
		}
	}
}

int mesh_wpad_full_wait(int root_fd, struct mesh_wpad_time *time)
{
	uint64_t logged_at = 0;
	int logged = 0;
	enum mesh_wpad_state previous = MESH_WPAD_ABSENT;

	if (time == NULL || time->now_ms == NULL || time->sleep_ms == NULL) {
		return -1;
	}
	for (;;) {
		enum mesh_wpad_state state;
		uint64_t now;
		int force = 0;
		if (stopped_without_reload(time)) {
			return -1;
		}
		if (time->reload != NULL && *time->reload) {
			consume_reload(time);
			force = 1;
		}
		state = mesh_wpad_full_state(root_fd);
		if (state == MESH_WPAD_FULL) {
			return 0;
		}
		now = time->now_ms(time->context);
		if (!logged || state != previous || now - logged_at >= MESH_WPAD_RECHECK_MS || force) {
			log_state(time, state);
			logged = 1;
			previous = state;
			logged_at = now;
		}
		if (time->sleep_ms(time->context, 1000) != 0) {
			return -1;
		}
	}
}
