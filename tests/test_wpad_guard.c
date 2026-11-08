/**
 * @file test_wpad_guard.c
 * @brief Host fixtures for the full-wpad startup guard.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "wpad_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int checks;
static unsigned int failures;

static void check(const char *name, int condition)
{
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "not ok %u - %s\n", checks, name);
		return;
	}
	printf("ok %u - %s\n", checks, name);
}

struct fixture {
	char *root;
	int fd;
	uint64_t now;
	unsigned int logs;
	char last[320];
	int stop_on_sleep;
	int become_full_on_sleep;
	unsigned int stop_after_logs;
	volatile sig_atomic_t *stop_flag;
	volatile sig_atomic_t *reload_flag;
	const char *full_name;
};

static int write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return -1;
	}
	if (fputs(text, file) == EOF) {
		fclose(file);
		return -1;
	}
	return fclose(file);
}

static int install_tree(struct fixture *fixture, const char *database, const char *package, int links)
{
	char path[512];
	if (snprintf(path, sizeof(path), "%s/lib/apk/db", fixture->root) >= (int)sizeof(path)) {
		return -1;
	}
	if (mkdir(fixture->root, 0700) != 0 && errno != EEXIST) {
		return -1;
	}
	snprintf(path, sizeof(path), "%s/lib", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lib/apk", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lib/apk/db", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lib/apk/packages", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/usr", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/usr/sbin", fixture->root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lib/apk/db/installed", fixture->root);
	if (database != NULL && write_file(path, database) != 0) {
		return -1;
	}
	if (package != NULL) {
		snprintf(path, sizeof(path), "%s/lib/apk/packages/%s.list", fixture->root, package);
		if (write_file(path, "/usr/sbin/wpad\n/usr/sbin/hostapd\n/usr/sbin/wpa_supplicant\n") != 0) {
			return -1;
		}
		snprintf(path, sizeof(path), "%s/usr/sbin/wpad", fixture->root);
		if (write_file(path, "wpad\n") != 0) {
			return -1;
		}
		if (links) {
			char hostapd[512];
			char supplicant[512];
			snprintf(hostapd, sizeof(hostapd), "%s/usr/sbin/hostapd", fixture->root);
			snprintf(supplicant, sizeof(supplicant), "%s/usr/sbin/wpa_supplicant", fixture->root);
			if (symlink("wpad", hostapd) != 0 || symlink("wpad", supplicant) != 0) {
				return -1;
			}
		}
	}
	fixture->fd = open(fixture->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	return fixture->fd < 0 ? -1 : 0;
}

static void fixture_open(struct fixture *fixture, const char *name)
{
	char pattern[] = "/tmp/mesh-wpad-XXXXXX";
	memset(fixture, 0, sizeof(*fixture));
	fixture->root = mkdtemp(pattern);
	if (fixture->root != NULL) {
		fixture->root = strdup(fixture->root);
	}
	fixture->full_name = name;
	fixture->fd = -1;
}

static uint64_t now_ms(void *context)
{
	return ((struct fixture *)context)->now;
}

static int sleep_ms(void *context, unsigned int milliseconds)
{
	struct fixture *fixture = context;
	fixture->now += milliseconds;
	if (fixture->stop_on_sleep && fixture->stop_flag != NULL) {
		*fixture->stop_flag = 1;
	}
	if (fixture->become_full_on_sleep) {
		char path[512];
		snprintf(path, sizeof(path), "%s/lib/apk/db/installed", fixture->root);
		write_file(path, "P:wpad-mbedtls\nV:1\n\n");
		snprintf(path, sizeof(path), "%s/lib/apk/packages/wpad-mbedtls.list", fixture->root);
		write_file(path, "/usr/sbin/wpad\n");
		snprintf(path, sizeof(path), "%s/usr/sbin/wpad", fixture->root);
		write_file(path, "wpad\n");
		snprintf(path, sizeof(path), "%s/usr/sbin/hostapd", fixture->root);
		if (symlink("wpad", path) != 0 && errno != EEXIST) {
			return -1;
		}
		snprintf(path, sizeof(path), "%s/usr/sbin/wpa_supplicant", fixture->root);
		if (symlink("wpad", path) != 0 && errno != EEXIST) {
			return -1;
		}
		fixture->become_full_on_sleep = 0;
	}
	return 0;
}

static void capture(void *context, const char *line)
{
	struct fixture *fixture = context;
	fixture->logs++;
	snprintf(fixture->last, sizeof(fixture->last), "%s", line);
	if (fixture->stop_after_logs != 0 && fixture->logs >= fixture->stop_after_logs && fixture->stop_flag != NULL) {
		*fixture->stop_flag = 1;
	}
}

static void expect_state(const char *name, const char *database, const char *package, int links, enum mesh_wpad_state state)
{
	struct fixture fixture;
	fixture_open(&fixture, package);
	check(name, fixture.root != NULL && install_tree(&fixture, database, package, links) == 0 &&
		  mesh_wpad_full_state(fixture.fd) == state);
	if (fixture.fd >= 0) {
		close(fixture.fd);
	}
	if (fixture.root != NULL) {
		/* The directory is tiny and disposable; leave removal to the process exit temp lifetime. */
		free(fixture.root);
	}
}

int main(void)
{
	struct fixture fixture;
	struct mesh_wpad_time time;
	volatile sig_atomic_t stop = 0;
	volatile sig_atomic_t reload = 0;
	const char *peer;
	char *source;
	size_t length;
	FILE *file;
	static const char *full_names[] = { "wpad-mbedtls", "wpad-openssl", "wpad-wolfssl" };
	size_t i;

	printf("TAP version 13\n");
	expect_state("absent database is not a full wpad", NULL, NULL, 0, MESH_WPAD_ABSENT);
	expect_state("unsuffixed internal wpad is rejected", "P:wpad\n", "wpad", 1, MESH_WPAD_INTERNAL);
	expect_state("basic mbedtls is rejected", "P:wpad-basic-mbedtls\n", "wpad-basic-mbedtls", 1, MESH_WPAD_BASIC);
	expect_state("basic openssl is rejected", "P:wpad-basic-openssl\n", "wpad-basic-openssl", 1, MESH_WPAD_BASIC);
	expect_state("mini is rejected", "P:wpad-mini\n", "wpad-mini", 1, MESH_WPAD_MINI);
	expect_state("mesh-only mbedtls is rejected", "P:wpad-mesh-mbedtls\n", "wpad-mesh-mbedtls", 1, MESH_WPAD_MESH_ONLY);
	expect_state("mesh-only openssl is rejected", "P:wpad-mesh-openssl\n", "wpad-mesh-openssl", 1, MESH_WPAD_MESH_ONLY);
	expect_state("mesh-only wolfssl is rejected", "P:wpad-mesh-wolfssl\n", "wpad-mesh-wolfssl", 1, MESH_WPAD_MESH_ONLY);
	expect_state("malformed package name is rejected", "P:wpad-mbedtls;touch\n", NULL, 0, MESH_WPAD_MALFORMED);
	expect_state("full name without the multicall binary is partial", "P:wpad-mbedtls\n", NULL, 0, MESH_WPAD_PARTIAL);
	for (i = 0; i < sizeof(full_names) / sizeof(full_names[0]); i++) {
		char database[64];
		char label[80];
		snprintf(database, sizeof(database), "P:%s\nV:1\n\n", full_names[i]);
		snprintf(label, sizeof(label), "full package %s is accepted", full_names[i]);
		expect_state(label, database, full_names[i], 1, MESH_WPAD_FULL);
	}
	fixture_open(&fixture, "wpad-mbedtls");
	install_tree(&fixture, "P:wpad-basic-mbedtls\n", "wpad-basic-mbedtls", 1);
	memset(&time, 0, sizeof(time));
	time.context = &fixture;
	time.now_ms = now_ms;
	time.sleep_ms = sleep_ms;
	time.stop = &stop;
	time.reload = &reload;
	time.log_line = capture;
	fixture.become_full_on_sleep = 1;
	fixture.stop_flag = &stop;
	check("installing full wpad during the wait allows startup", mesh_wpad_full_wait(fixture.fd, &time) == 0);
	check("the blocked wait logged the basic rejection once", fixture.logs == 1 && strstr(fixture.last, "basic") != NULL);
	close(fixture.fd);
	free(fixture.root);

	fixture_open(&fixture, "wpad-basic-mbedtls");
	install_tree(&fixture, "P:wpad-basic-mbedtls\n", "wpad-basic-mbedtls", 1);
	fixture.logs = 0;
	fixture.stop_on_sleep = 1;
	fixture.stop_flag = &stop;
	stop = 0;
	time.context = &fixture;
	check("stop during the wait does not continue", mesh_wpad_full_wait(fixture.fd, &time) != 0 && fixture.logs == 1);
	close(fixture.fd);
	free(fixture.root);

	fixture_open(&fixture, "wpad-basic-mbedtls");
	install_tree(&fixture, "P:wpad-basic-mbedtls\n", "wpad-basic-mbedtls", 1);
	fixture.logs = 0;
	fixture.stop_on_sleep = 0;
	fixture.stop_after_logs = 2;
	fixture.stop_flag = &stop;
	stop = 0;
	reload = 0;
	time.context = &fixture;
	check("repeated rejection is logged again at 60 seconds, not every second",
		  mesh_wpad_full_wait(fixture.fd, &time) != 0 && fixture.logs == 2 && fixture.now >= 60000);
	close(fixture.fd);
	free(fixture.root);

	file = fopen("src/mesh11sd/peer_main.c", "r");
	if (file == NULL) {
		file = fopen("../src/mesh11sd/peer_main.c", "r");
	}
	if (file == NULL) {
		file = fopen("../../src/mesh11sd/peer_main.c", "r");
	}
	source = NULL;
	length = 0;
	if (file != NULL) {
		fseek(file, 0, SEEK_END);
		length = (size_t)ftell(file);
		rewind(file);
		source = malloc(length + 1);
		if (source != NULL && fread(source, 1, length, file) == length) {
			source[length] = '\0';
		}
		fclose(file);
	}
	peer = source != NULL ? strstr(source, "mesh_wpad_full_wait") : NULL;
	check("startup guard is wired before the live bus",
		  peer != NULL && strstr(peer, "mesh_live_bus_open") != NULL &&
		  strstr(source, "mesh_live_bus_open") > peer);
	check("--version remains ahead of the guard",
		  source != NULL && strstr(source, "\"--version\"") != NULL && strstr(source, "\"--version\"") < peer);
	free(source);
	printf("1..%u\n", checks);
	printf("# %u wpad guard checks, %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
