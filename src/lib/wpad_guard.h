/**
 * @file wpad_guard.h
 * @brief Read-only check that a full wpad multicall package is installed.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH11SD_WPAD_GUARD_H
#define MESH11SD_WPAD_GUARD_H

#include <signal.h>
#include <stdint.h>

/* Recognized full packages are exactly wpad-mbedtls, wpad-openssl, and
 * wpad-wolfssl. OpenWrt 25.12.6 enables CONFIG_MESH and CONFIG_OWE only for
 * those external TLS full variants. Unsuffixed wpad is wpad-full-internal and
 * does not get those flags. basic, mini, and mesh-only names are rejected.
 * A custom build under another name is unsupported. ubus get_features is not
 * used: it reports HT/VHT only. The apk database must name the package and
 * lib/apk/packages/<name>.list must list /usr/sbin/wpad, which must be a
 * regular file with hostapd and wpa_supplicant linked to wpad.
 */
enum mesh_wpad_state {
	MESH_WPAD_FULL = 0,
	MESH_WPAD_ABSENT,
	MESH_WPAD_INTERNAL,
	MESH_WPAD_BASIC,
	MESH_WPAD_MINI,
	MESH_WPAD_MESH_ONLY,
	MESH_WPAD_PARTIAL,
	MESH_WPAD_MALFORMED
};

struct mesh_wpad_time {
	void *context;
	uint64_t (*now_ms)(void *context);
	int (*sleep_ms)(void *context, unsigned int milliseconds);
	volatile sig_atomic_t *stop;
	volatile sig_atomic_t *reload;
	void (*log_line)(void *context, const char *line);
};

enum mesh_wpad_state mesh_wpad_full_state(int root_fd);
/* Returns 0 when a full wpad is present. Returns -1 when stop is requested
 * before that, without creating network state. Identical failures are logged
 * at most once per 60 seconds. SIGHUP (reload set, stop set) rechecks at once.
 */
int mesh_wpad_full_wait(int root_fd, struct mesh_wpad_time *time);

#endif
